---
title: Troubleshooting
description: Resolve installation problems, read a run-time report, find the bug behind a heap, stack or global report or an invalid release, handle code that reads past objects on purpose, understand the link notes, and deal with slow programs and the advisory analysis.
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

## A program built with weavec-cc traps

A trap (`SIGTRAP`, or `SIGILL` on x86-64; a shell shows exit status 133 or 132) means a guard found an access to memory that is not addressable, and the program stopped before making it. The runtime prints one line on standard error first:

```text
weavec: heap-buffer-overflow at vec.c:6:43: read of 4 bytes at 0xbe00000010
weavec: 0xbe00000010 is 0 bytes after the 16-byte heap object at 0xbe00000000
```

The line names the kind of bad access, the source location of the access (`<unknown>` when the unit has no location for it), whether it reads or writes, its width and its address. A report on the heap adds a second line that places the address relative to the heap object it hit. An index check has its own form, `weavec: index-out-of-bounds at lookup.c:8:12: index -1`, and an access with no known width reads `access at 0x…`.

To see every failing site in one run, rebuild with `-fweavec-checks=report` and rerun: each failure is printed once per site and the program goes on (what it does after an invalid access is not defined). `WEAVEC_RT_ABORT=1` makes a report-mode build trap at the first failure again. `WEAVEC_RT_REPORT_LOG=<path>` appends the reports to that file instead of standard error, which helps when a test harness hides a passing test's output; a set-user-ID program ignores it. Run the program under a debugger to see the call stack at the trap.

A trap is almost always a real bug. The exceptions are programs that read past an object on purpose, described [below](#code-that-reads-past-an-object-on-purpose), and an exception unwound through C frames by code built without WeaveC, which can leave a frame's redzones behind for a later guard to meet (see [assumptions](/reference/guarantees/#assumptions)). See [false traps](/reference/guarantees/#false-traps) for what the guarantee accepts.

## Heap reports

- `heap-buffer-overflow`: the access leaves a heap block past the size the program asked for. The second line says how far past the end (`is 0 bytes after the 16-byte heap object`) the address is. Look for an index or length that exceeds the allocation, an allocation sized in elements where bytes were meant (or the reverse), or a missing byte for a string's terminator. The runtime's allocator gives each block exactly its requested size: slack a system allocator rounds up to is not addressable, and `malloc_usable_size` returns the requested size.
- `heap-use-after-free`: the access is to a block that was freed and is still in the quarantine (`is inside a released heap block`). Look for a pointer kept across a `free`, or across a `realloc` that moved the block. Once the quarantine (16 MiB of released blocks by default) recycles a block, a stale pointer into it sees the new object and is not caught; a larger `WEAVEC_RT_QUARANTINE=<bytes>` keeps blocks longer.

## Stack and global reports

- `stack-buffer-overflow`: the access left a local into its redzone. Every local a guard can reach is followed by at least 32 bytes no object owns.
- `dynamic-stack-buffer-overflow`: the same for an `alloca` block or a variable-length array.
- `stack-use-after-scope`: the access is to a local whose block has ended, through a pointer that outlived it. This is caught at every optimisation level. A use after the function returned is not caught.
- `global-buffer-overflow`: the access left a global or static variable the unit defines into its redzone.

An access that jumps over a redzone into the next object passes, as it would under AddressSanitizer. Within a struct, an index into a declared array field is checked against its bound (`index-out-of-bounds`); a pointer walk past a field is not.

## Other reports

- `index-out-of-bounds`: `a[i]` into an array whose bound the type declares, with `i` outside it. A multidimensional array indexed past an inner bound (`m[0][k]` with `k` at least the inner size) is reported too; index the flattened storage through a pointer instead.
- `null-dereference`: the access is through a null pointer at an offset the guard could not rule out, or to the lowest 64 KiB of the address space. A string argument that is null where the library function requires a string is reported the same way.
- `unterminated-string`: a library call that reads a string (`strlen`, `strcpy`, a `%s` conversion) ran off the end of addressable memory without finding the terminator.
- `overlapping-copy`: the source and destination of `memcpy` or another function that requires disjoint operands overlap; use `memmove`.
- `buffer-overflow` and `invalid-access`: the address is not addressable, but the runtime cannot say which kind of object it belongs to.
- `weavec.proven: <kind>`: only in a `-fweavec-checks=verify` build. A guard that a local rule removed would have failed, so the rule was wrong. Please [report it](#report-an-issue) with the source.

## The program stops with "weavec: invalid release"

```text
weavec: invalid release of 0xbd00000000: the block was already released
```

The runtime's allocator received a `free` or `realloc` it cannot honour, and stopped the program. This line has no source location, because the allocator validates the release, not a guard. The reasons are:

| Reason                                                        | Usual cause                                                                                                                          |
| ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `the block was already released`                              | a double free                                                                                                                        |
| `not the start of its block`, `not the start of a live block` | a free of an interior pointer                                                                                                        |
| `no block is allocated there`                                 | a free of a pointer into the heap that no block starts at, or of a stale pointer after the block was recycled                        |
| `not a heap block`                                            | a free of a stack, global or other pointer that no allocator owns (on Linux the C library's allocator rejects such a pointer itself) |

Run the program under a debugger to find the caller of `free`. A program that defines its own allocator does not get this check (see [below](#the-link-prints-defines-the-allocator)).

## Code that reads past an object on purpose

Some correct-looking code reads past the end of an object deliberately: string functions that load a word at a time, hash functions that read whole words of a key (json-c's `hashlittle`), conservative garbage collectors that scan the stack. It traps under WeaveC as it does under AddressSanitizer. Use the project's own switch if it has one (json-c's `PRECISE_MEMORY_ACCESS`), or keep the code out of the guards:

```c
#include <weavec.h>

WEAVEC_UNSAFE static unsigned long hash_words(const unsigned long *p, size_t n) {
    /* reads whole words past the key on purpose */
}
```

Accesses in a `WEAVEC_UNSAFE` function or block are not guarded and its array indexes are not checked. A function marked `__attribute__((no_sanitize("address")))`, `no_sanitize_address` or `disable_sanitizer_instrumentation`, as code written for AddressSanitizer already is, is treated the same way, so such code needs no change. Keep the region to the code that needs it; see [unsafe boundaries](/guides/unsafe/).

## The link prints "building without the WeaveC runtime"

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): memory accesses are not guarded
```

The command line has something the runtime cannot work with: a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak`, `kernel-address`), `-ffreestanding`, `-nostdlib`, `-nodefaultlibs`, `-nolibc`, or a target other than 64-bit Darwin or Linux. The units are then compiled as with `-fweavec-checks=none`: Clang's output, no guards and no runtime. Nothing is wrong with the build; the note records that it enforces nothing. Remove the flag, or pass `-fweavec-checks=none` yourself to say so explicitly. See [without the runtime](/reference/guarantees/#without-the-runtime).

## The link prints "defines the allocator"

```text
weavec-cc: note: 'alloc.o' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated
```

An object or archive member on the link line defines `malloc`, `calloc`, `realloc` or `free`, so the program keeps its own allocator. Guards on locals and globals still work; accesses to the heap pass, and no heap overflow, use after free or invalid free is caught. To have the heap tracked, build without the program's allocator if it has a switch for that.

## The program warns that the shadow memory could not be reserved

```text
weavec: warning: the shadow memory could not be reserved; memory accesses are not checked
```

At start-up the runtime reserves address space for its shadow and its arena, without committing memory. A limit on virtual memory (`ulimit -v`, `RLIMIT_AS`) smaller than the reservation makes it fail, and the program then runs with every guard passing. Raise or remove the limit for guarded programs. The similar warning that the heap lies outside the shadow's window means the arena could not be placed where the shadow covers it; heap accesses are then not checked.

## The program is slower or uses more memory

A guarded program reads the runtime's shadow memory (one byte per 16 bytes of the program's memory) before each access the guard passes could not prove safe, gives tracked locals and globals redzones, and holds freed blocks in a quarantine before reusing them. [RFC 0035's acceptance gates](/rfcs/0035-guard-by-default/#acceptance-gates) set targets for that cost: at most twice the run time of a plain Clang build, or AddressSanitizer's ratio where that is larger, and 1.8 times on average. They are targets; the RFC records the measurements when its gates are run.

- Run with `WEAVEC_RT_STATS=1` to see the runtime's counters at exit: how many guards reached the runtime's slow path (`slow guards`), how many range and string guards ran, and how much the allocator did.
- Write the ledger (`-fweavec-ledger=<dir>/ -fweavec-summary`) and look at the guarded share: the summary line (`812 accesses: 431 proven, 381 guarded, 0 unguarded`) and the rows say which accesses in a hot function kept their guard. A loop with a known trip count of at least 16 iterations, one exit and nothing that may end a lifetime runs a copy without the guards of its invariant and affine accesses once one range check passes; other loops keep their guards.
- Set `WEAVEC_RT_QUARANTINE=<bytes>` to shrink the quarantine if memory is the problem. A smaller quarantine catches fewer uses of freed blocks; `0` reuses blocks at once.
- As a last resort, put a reviewed hot function in a `WEAVEC_UNSAFE` region, or build a unit that cannot pay with `-fweavec-checks=none`. Both give up the guarantee for that code.

## Headers or macros are missing during analysis

Use a current `compile_commands.json` with `-p build`, or pass your actual include directories and defines after `--`:

```sh
weavec src/main.c -- -std=c17 -Iinclude -DPROJECT_FEATURE=1
```

Resolve Clang parse errors before interpreting analysis results.

## weavec reports errors but the build succeeds

The ownership and lifetime analysis is advisory: it never changes the generated code, and `weavec-cc` does not run it by default. The `weavec` tool reports a definite finding as an error and exits with status 1; the build, which relies on its guards, is unaffected. To fail a build on the analysis's findings, run `weavec` in CI, or compile with `-fweavec-diagnose -Werror=weavec`: `-fweavec-diagnose` prints every finding as a warning, and `-Werror=weavec` makes them errors. See [the advisory analysis](/reference/cli/#the-advisory-analysis).

## A helper's behaviour is not visible to the analysis

Analyse its source with the caller using `weavec --whole-program`, which checks each call against the callee's definition in another file. `weavec-cc -fweavec-diagnose` analyses one unit at a time. An unannotated declaration alone does not describe an unavailable function: the analysis assumes it may free, keep or replace its pointer arguments, and the sites that depend on them count as not proven in the summary line.

For callbacks, ensure the actual targets are stored somewhere the program can see. A function-pointer type alone does not say which function a call reaches.

## The analysis is slow

Each function's analysis has a work budget, `--budget=<n>` in `weavec` and `-fweavec-budget=<n>` with `-fweavec-diagnose` (default 20,000,000; `0` for no limit), and each unit's analysis has one over all of its functions (by default 400 per site, at least 20,000,000; `-fweavec-unit-budget=<n>` with `-fweavec-diagnose`). The sites of a function whose analysis ran out of budget count as not proven. Lower the budgets for faster runs on very large units; see [budgets](/reference/cli/#budgets). `--analysis-stats=<path>` writes where the work went. The budgets do not affect the guards.

## Report an issue

Include the WeaveC and LLVM versions, platform, exact command, a minimal source example, and the complete diagnostic or run-time report. Remove private source and environment details you do not intend to publish. Open a [bug report](https://github.com/weavefoundry/weavec/issues/new?template=bug_report.yml).
