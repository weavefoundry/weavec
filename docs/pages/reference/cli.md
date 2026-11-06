---
title: Command-line reference
description: weavec-cc options for guard modes, zero-initialization, AddressSanitizer compatibility, fortification, the runtime and its environment variables, the enforcement ledger and its summary line, and the options that were removed.
---

WeaveC is one tool, `weavec-cc`: Clang's compiler driver with WeaveC inside. It guards every memory access of the C code it compiles and links the runtime that the guards call. The options below are specified by [RFC 0035](/rfcs/0035-guard-by-default/) §7 as amended by [RFC 0036](/rfcs/0036-deterministic-heap-reuse/) §1, §6 and §7; `weavec-cc --help-weavec` lists the ones your build accepts.

## Invocation

```sh
weavec-cc [clang options] [weavec-cc options] source.c -o program
make CC=weavec-cc
```

`weavec-cc` accepts every Clang driver option, so a build system uses it as it would use `cc`.

## weavec-cc

| Flag                                               | Default                          | Meaning                                                                                                                                                                                                                                                                                                  |
| -------------------------------------------------- | -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-fweavec` / `-fno-weavec`                         | on                               | Guard memory accesses. Off compiles as plain Clang.                                                                                                                                                                                                                                                      |
| `-fweavec-checks=trap\|report\|verify\|none`       | `trap`                           | What a failed guard does; see [check modes](#check-modes).                                                                                                                                                                                                                                               |
| `-fweavec-zero-init` / `-fno-weavec-zero-init`     | on unless checks are `none`      | Zero-initialize locals of up to 4 KiB (`-ftrivial-auto-var-init=zero`; a `-ftrivial-auto-var-init-max-size` on the command line sets another limit). The runtime's allocator zero-fills every block regardless.                                                                                          |
| `-fweavec-asan-compat` / `-fno-weavec-asan-compat` | on                               | Compile and preprocess C as AddressSanitizer would for the source; see [AddressSanitizer compatibility](#addresssanitizer-compatibility).                                                                                                                                                                |
| `-fweavec-colors` / `-fno-weavec-colors`           | on                               | Check an access computed from a pointer against the color of the pointer's object, so that an index or pointer that jumps over a redzone into a live neighbor traps ([guarantee (C)](/reference/guarantees/#the-guarantee)). Off, the shadow is colored all the same and only redzones stop such a jump. |
| `-fweavec-ledger=<path>`                           | unset                            | Write the [enforcement ledger](#ledger-and-summary-line). A value ending in `/`, or naming an existing directory, receives `<object>.ledger.json` per unit; a file receives one ledger, and naming one for an invocation that compiles several C files is an error.                                      |
| `-fweavec-summary` / `-fno-weavec-summary`         | off, on when a ledger is written | Print the unit's summary line on stderr.                                                                                                                                                                                                                                                                 |
| `-fweavec-rpath=<dir>` / `-fno-weavec-rpath`       | the toolchain's `lib/weavec`     | Where a linked program or library finds [the runtime's library](#the-runtime): `<dir>` (`$ORIGIN`, `@loader_path`, an installation path) instead of the toolchain's runtime directory, or no rpath at all, for a package that installs `libweavec_rt` beside the program.                                |
| `--help-weavec`                                    |                                  | List the WeaveC flags and exit.                                                                                                                                                                                                                                                                          |

An unknown `-fweavec-*` flag is an error. Every other flag is Clang's. When WeaveC is on, the driver defines `__WEAVEC__=1` and puts the directory of `weavec.h` on the system include path. `weavec-cc --version` prints Clang's version block first, then `weavec-cc version <version> (<revision>)`, the LLVM it was built with, and the runtime's interface version and the platform (`runtime ABI 38, for arm64-apple-darwin…`), so a configure script that reads the first line sees Clang. Clang's headers and builtins are those of the toolchain (`lib/clang/<major>` beside `bin/`). Assembly sources (`.s`, `.S`) and `-save-temps` compiles are assembled by the system assembler (`as`), since Clang's integrated assembler is not part of `weavec-cc`. On macOS a link with `-flto` uses the system linker's libLTO, which may not read bitcode of a newer LLVM.

C++, Objective-C, OpenCL and CUDA sources are compiled as Clang compiles them, without guards; so is everything under `-fno-weavec` or `-fweavec-checks=none`.

### Fortification

On glibc targets, an optimizing compile in an enforcing mode whose command line neither defines nor undefines `_FORTIFY_SOURCE` gets `-D_FORTIFY_SOURCE=3`, so that the C library's headers pass each string and `printf`-family call the size of the object or field it writes; the guards check that size ([guarantee (F)](/reference/guarantees/#the-guarantee)). A command line that sets its own level, `-D_FORTIFY_SOURCE=0` included, keeps it. Darwin's SDK fortifies optimizing compiles at level 2 by itself.

### AddressSanitizer compatibility

By default, in the enforcing modes, `weavec-cc` parses each C unit as AddressSanitizer would: `__SANITIZE_ADDRESS__` is defined, `__has_feature(address_sanitizer)` is true and `no_sanitize("address")` is accepted. AddressSanitizer's own instrumentation never runs. Code written to be clean under ASan, such as a pool allocator that poisons its free space with `ASAN_POISON_MEMORY_REGION` or a scan that reads past an object only when ASan is off, is then clean under WeaveC. On Darwin, where Clang turns fortification off under ASan, the SDK's fortification stays on.

The runtime implements ASan's poisoning interface with ASan's meaning: `__asan_poison_memory_region` (whole 16-byte granules inside the region become not addressable, and an access to them reports `use-after-poison`), `__asan_unpoison_memory_region`, `__asan_address_is_poisoned`, `__asan_region_is_poisoned` and `__asan_describe_address`. It accepts and ignores `__asan_handle_no_return`, the fake-stack queries (there is no fake stack), `__sanitizer_print_stack_trace`, `__sanitizer_set_report_path`, `__sanitizer_annotate_contiguous_container`, the fiber-switch hooks, and LeakSanitizer's `__lsan_*` entry points (there is no leak checking).

`-fno-weavec-asan-compat` keeps the language options as the command line set them, for code whose ASan paths call a sanitizer interface the runtime does not implement (a link error) or behave differently in a way the program does not want. A unit built with a sanitizer of its own (`-fsanitize=address` and the like) is [built without the runtime](#the-runtime).

## Check modes

| `-fweavec-checks=` | A failed guard or index check                      | Guards removed by a local rule | Zero-init default | Runtime    | Guarantee                     |
| ------------------ | -------------------------------------------------- | ------------------------------ | ----------------- | ---------- | ----------------------------- |
| `trap` (default)   | prints a report and traps                          | removed                        | on                | linked     | yes                           |
| `report`           | prints a report once per site; the program goes on | removed                        | on                | linked     | only with `WEAVEC_RT_ABORT=1` |
| `verify`           | prints a report and traps                          | kept as monitors               | on                | linked     | yes                           |
| `none`             | nothing is guarded                                 | n/a                            | off               | not linked | no                            |

A trap ends the program with `SIGTRAP`, or `SIGILL` on x86-64. Before it traps, the runtime unblocks both signals in the failing thread and resets a disposition the program set to ignore them, so the program ends even with the signals blocked; a handler the program installed still runs. The report is one line on stderr, and a second line places the address relative to its object when the runtime knows it:

```text
weavec: heap-buffer-overflow at vec.c:9:43: read of 4 bytes at 0xbd80000010
weavec: 0xbd80000010 is 0 bytes after the 16-byte heap object at 0xbd80000000
```

The first line names the kind, the source location of the access (`<unknown>` when the unit has none), whether it reads or writes, its width and its address; an access with no known width reads `access at 0x…`. The kinds are:

| Kind                            | The access                                                                                                                                                                                                                                              | Second line                                                                                                      |
| ------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| `heap-buffer-overflow`          | reaches the bytes after a heap object in its slot, or, computed from a pointer, another heap object of another color                                                                                                                                    | `… is <n> bytes after the <size>-byte heap object at …`, or `… is <n> bytes after its base …, in another object` |
| `heap-use-after-free`           | reaches a released heap block                                                                                                                                                                                                                           | `… is inside a released heap block at …`                                                                         |
| `stack-buffer-overflow`         | reaches a redzone of a local                                                                                                                                                                                                                            |                                                                                                                  |
| `dynamic-stack-buffer-overflow` | reaches the redzone of an `alloca` block or a variable-length array                                                                                                                                                                                     |                                                                                                                  |
| `stack-use-after-scope`         | reaches a local whose block has ended                                                                                                                                                                                                                   |                                                                                                                  |
| `stack-use-after-return`        | reaches a local of a function that returned, before another frame reused its bytes                                                                                                                                                                      | `… is in a frame that returned`                                                                                  |
| `global-buffer-overflow`        | reaches the redzone of a global the unit defines                                                                                                                                                                                                        |                                                                                                                  |
| `intra-object-overflow`         | a fortified call writes past the size of the field or object its `_chk` form names                                                                                                                                                                      | `… is <n> bytes inside the <size>-byte destination at …`                                                         |
| `use-after-poison`              | reaches bytes the program poisoned through ASan's interface                                                                                                                                                                                             |                                                                                                                  |
| `null-dereference`              | is through a null pointer, or reaches the lowest 64 KiB                                                                                                                                                                                                 |                                                                                                                  |
| `unterminated-string`           | a string argument of a library call has no terminator in its object (a search that found nothing read up to one)                                                                                                                                        |                                                                                                                  |
| `overlapping-copy`              | the operands of `memcpy` or another call that requires disjoint operands overlap                                                                                                                                                                        |                                                                                                                  |
| `index-out-of-bounds`           | an index into an array whose bound the type declares is outside it: `weavec: index-out-of-bounds at lookup.c:8:12: index -1`                                                                                                                            |                                                                                                                  |
| `invalid-access`                | is to bytes that are not addressable, of no kind the runtime can name                                                                                                                                                                                   |                                                                                                                  |
| `memory-fault`                  | faulted where the shadow saw nothing to stop (a wild address, the inaccessible part of a huge block): `weavec: memory-fault at pc 0x…: read at 0x…`, after which the program dies of the signal as it would have (`WEAVEC_RT_FAULTS=0` reports nothing) | where the runtime places the address, as for the heap                                                            |

An invalid `free` is stopped by the allocator, which knows no source location: `weavec: invalid release of 0x…: the block was already released` (or `not the start of its block`, `not the start of a live block`, `no block is allocated there`, `not a heap block`).

In report mode each failing site (kind, file, line and column) prints once and the program continues, with no guarantee; an invalid release still stops it, since the allocator cannot go on from one. Verify mode is the monitor of the removal rules: every guard a local rule removed is emitted too, and its failure prints `weavec: weavec.proven: <kind> at …` before the trap, which means WeaveC removed a guard it needed. Verify mode does not version loops.

## The runtime

The runtime is one shared library, `libweavec_rt.so.<abi>` on Linux (soname `libweavec_rt.so.<abi>`) and `libweavec_rt.<abi>.dylib` on Darwin (install name `@rpath/libweavec_rt.<abi>.dylib`), so that a program never loads a runtime of another ABI, in the toolchain's `lib/weavec`. It holds the shadow memory, the arena allocator with `malloc`, `calloc`, `realloc`, `free` and the other standard allocation functions, the sweep, the guards' slow paths, the checked library wrappers, ASan's interface and the reports, and exports only those. Every link in the `trap`, `report` and `verify` modes, of a program, a shared library (`-shared`, `-dynamiclib`) or a plug-in (`-bundle`), adds the library before the first library on the line, an rpath to its directory (or to [`-fweavec-rpath`](#weavec-cc)'s), the runtime's archive `libweavec_rt.a`, and `-lpthread` on Linux. The link takes nothing from the archive; it is there for a link another driver makes from what it reads in this one (CMake links a target with C and C++ sources with `c++`, adding the C compiler's libraries), whose program then carries the runtime itself. The archive is never loaded whole: inside `-Wl,--whole-archive` the driver closes the region around it, and under `-Wl,-all_load` it leaves the archive out. A static Linux program (`-static`, `-static-pie`) takes the archive `libweavec_rt.a` instead, with no rpath. A relocatable link (`-r`) gets nothing. Link through `weavec-cc` so that it can add the library; a link by another driver fails with undefined `__weavec_rt_` symbols. [Safety guarantees](/reference/guarantees/) states what the guards promise.

Every image a process loads that `weavec-cc` linked depends on the library, so the dynamic loader loads it once, before them, and one runtime serves the process: one shadow, one heap, one quarantine, whatever the images' version scripts, `-Bsymbolic` or link order. On Linux the library comes before the C library in the loader's search order, so its `malloc` serves every image and the C library's own allocations (`strdup`, `getline`, `asprintf`); on Darwin the images `weavec-cc` linked bind to its allocator and the rest of the process reaches the arena through its malloc zone. A program that runs a guarded image elsewhere (on another machine, from a package) needs the library too, as a program built with ASan's dynamic runtime does: install it beside the program and link with `-fweavec-rpath='$ORIGIN'` (Linux) or `-fweavec-rpath=@loader_path` (Darwin).

When a program that `weavec-cc` did not link loads a guarded shared library on Linux (a Python extension, a PostgreSQL module), the C library's allocator was bound first and keeps the process's heap; the runtime leaves it alone, checks the guarded image's frames and globals, and says once that the heap is not:

```text
weavec: note: the program's allocator is not WeaveC's (/lib/x86_64-linux-gnu/libc.so.6 serves it: the program was not linked by weavec-cc): heap accesses are not checked; preload <toolchain>/lib/weavec/libweavec_rt.so.38 to check them
```

`LD_PRELOAD=<toolchain>/lib/weavec/libweavec_rt.so.38` makes the runtime the process's allocator from the start; `WEAVEC_RT_NOTES=0` silences the note. Images built by two WeaveC versions (two runtimes of different interface versions) cannot share a process, which stops at the second one's load with:

```text
weavec: error: two WeaveC runtimes in one process (<first> and <second>): build every image with one weavec-cc
```

`weavec-cc` compiles without guards and links without the runtime, as if `-fweavec-checks=none` had been given, when the command line has `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak` or `kernel-address`), or a target other than 64-bit Darwin or Linux. A link prints one of:

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): memory accesses are not guarded
weavec-cc: note: building without the WeaveC runtime (-nostdlib links no C library): memory accesses are not guarded
weavec-cc: note: building without the WeaveC runtime (-ffreestanding has no hosted C library): memory accesses are not guarded
weavec-cc: note: building without the WeaveC runtime (the runtime supports 64-bit Darwin and Linux targets): memory accesses are not guarded
```

When a link input, an object or a member of an archive, defines `malloc` itself, the program keeps its allocator: the loader binds `malloc` to the program's definition before the runtime's. The guards stay, but the heap is untracked:

```text
weavec-cc: note: 'alloc.o' defines the allocator, which serves the program instead of the WeaveC runtime's: the heap is untracked, guards pass on it and releases are not validated
```

A program's own definition of another allocation function (`posix_memalign`, `reallocarray` and the like) over `malloc` replaces the runtime's and keeps the arena. A `free` that reaches the runtime's allocator from code another compiler built is still validated.

A released block is reused only after a _sweep_ finds no pointer to it ([guarantee (T)](/reference/guarantees/#the-guarantee)). A sweep stops every other thread of the process, scans the threads' registers (vector registers too), stacks and thread-specific data, every readable mapping of the process but code and shared files it cannot write, and every live heap block for words that point into released blocks, and returns the blocks nothing points to to the free lists; blocks something still points to stay released until a later sweep. With 64 MiB or more of live heap the sweep's mark is shared with up to seven worker threads of the runtime's (one fewer than the processors), which are never stopped and hold no program pointer. It runs when the bytes released since the last sweep exceed `WEAVEC_RT_QUARANTINE` and a quarter of the live heap (the soft budget) and the last sweep ended at least eight of its durations ago, so that sweeps take at most about a ninth of the run time while the hard budget is not reached; when they exceed twice `WEAVEC_RT_QUARANTINE` and half the live heap (the hard budget), at once; and when a size class runs out of space after a quarter of its region was released into it (otherwise the class takes a spare region). Between sweeps the released memory waits. Each thread keeps up to 32 free and 32 released slots of each small size class to itself; the released ones are quarantined as any other and join the shared quarantine when the thread's cache fills, when it sweeps, and when it exits. On Darwin the sweep stops threads with `thread_suspend`; on Linux it signals each thread. The sweeping thread blocks signals while it runs, so that a handler that allocates does not wait for the sweep it interrupted.

| Environment variable           | Meaning                                                                                                                                                                                                                                                                                                                              |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `WEAVEC_RT_QUARANTINE=<bytes>` | The bytes of released blocks, gathered since the last sweep, that may start a sweep once they also exceed a quarter of the live heap; twice as many, and half the live heap, start one at once. Default 64 MiB (67108864). A smaller value sweeps more often and holds less memory; no value lets a block be reused without a sweep. |
| `WEAVEC_RT_SWEEP_SIGNAL=<n>`   | Linux only: the signal number the sweep stops threads with. By default `SIGPWR`, or `SIGXCPU` when the program handles `SIGPWR`.                                                                                                                                                                                                     |
| `WEAVEC_RT_SWEEP_THREADS=<n>`  | The mark workers a sweep of a large heap uses, at most 7; `0` marks on the sweeping thread alone. By default one fewer than the processors, up to 7.                                                                                                                                                                                 |
| `WEAVEC_RT_FAULTS=0`           | Install no handlers for `SIGSEGV` and `SIGBUS`, so that a fault the shadow did not see is not reported as `memory-fault`. The handlers are installed only where the disposition is the default, and a handler the program installs replaces them.                                                                                    |
| `WEAVEC_RT_ABORT=1`            | In report mode, trap at the first failure as trap mode does.                                                                                                                                                                                                                                                                         |
| `WEAVEC_RT_BACKTRACE=1`        | After the report of an access that stops the program, print its call stack on stderr.                                                                                                                                                                                                                                                |
| `WEAVEC_RT_REPORT_LOG=<path>`  | Append every report line to this file instead of printing it on stderr, so a test that captures stderr sees only the program's own output. Ignored by set-user-ID and set-group-ID programs.                                                                                                                                         |
| `WEAVEC_RT_STATS=1`            | At exit, print the runtime's counters on stderr, one per line, as `weavec: runtime: <n> <what>`. The process's one runtime counts every image's work.                                                                                                                                                                                |
| `WEAVEC_RT_NOTES=0`            | Do not print the note that the process's allocator is not the runtime's.                                                                                                                                                                                                                                                             |

The counters of `WEAVEC_RT_STATS=1` are allocations, releases, recycled slots (released slots a sweep returned for reuse), huge blocks, slow guards (the guards whose inline check sent them to the runtime), range guards, string guards, stack unpoisons (clearings of the stack's shadow before a call that does not return) and sweeps. Two last lines give the sweeps' total and longest pause and what the last sweep had to keep, and the most the live heap's slots and the released blocks waiting for a sweep held:

```text
weavec: runtime: sweeps took 48 ms (longest 7 ms); the last retained 0 blocks (0 bytes)
weavec: runtime: at most 86 MiB of live slots and 82 MiB of released ones waiting for a sweep
```

The counters are not synchronized between threads.

## Ledger and summary line

The enforcement ledger records what the guard passes decided for each access of a unit. It is JSON, `weavec-ledger` version 3:

```json
{
  "schema": "weavec-ledger",
  "version": 3,
  "producer": { "name": "weavec-cc", "version": "0.16.0" },
  "units": [
    {
      "source": "lookup.c",
      "object": "lookup.o",
      "target": "arm64-apple-macosx15.0.0",
      "config": { "checks": "trap", "zeroInit": true },
      "summary": { "accesses": 3, "proven": 1, "guarded": 2, "unguarded": 0 },
      "rows": [
        {
          "function": "lookup",
          "file": "lookup.c",
          "line": 8,
          "column": 12,
          "operation": "load",
          "bytes": 4,
          "outcome": "guarded",
          "reason": "access"
        }
      ]
    }
  ]
}
```

A row's `operation` is `load`, `store`, `rmw` (an atomic operation), `copy`, `set`, `lane` (a lane of a masked or gathered access) or `call:<name>` (a library call's argument); `bytes` is 0 when the length is computed at run time. Its outcome and reason:

| Outcome     | Reason         | Meaning                                                                                  |
| ----------- | -------------- | ---------------------------------------------------------------------------------------- |
| `guarded`   | `access`       | An inline check of the bytes the access touches.                                         |
|             | `range`        | A check of a range whose length is computed at run time.                                 |
|             | `string`       | A string argument, checked up to its terminator.                                         |
|             | `checked-call` | The call goes through the runtime's checked version of the function.                     |
| `proven`    | `in-bounds`    | It lies inside a local or a global at an offset whose range is known (RFC 0035 §6.1).    |
|             | `dominated`    | A guard on the same pointer covers its bytes, with nothing between that may free (§6.2). |
|             | `merged`       | It is merged into one guard of nearby accesses on the same pointer (§6.3).               |
|             | `optimized`    | The optimizer removed every copy of the access.                                          |
| `unguarded` | `unsafe`       | It lies in a `WEAVEC_UNSAFE` region or a function excluded from AddressSanitizer.        |

The rows are collected when the guards are inserted, before the optimizer; an access to a local or a global at a constant offset inside it gets neither a guard nor a row. A loop whose whole range was checked once still counts its accesses as `guarded`. `-fweavec-summary` prints the unit's counts:

```text
weavec: lookup.c: 3 accesses: 1 proven, 2 guarded, 0 unguarded
```

In a Makefile, pass a directory so parallel compiles write separate files:

```sh
make CC=weavec-cc CFLAGS="-O2 -fweavec-ledger=build/ledger/"
```

Every ledger is written to a temporary file and renamed into place. A link writes no ledger.

## Removed options

RFC 0036 deleted the advisory ownership and lifetime analysis, with no aliases. The `weavec` tool is gone with all of its options (`--whole-program`, `--dump-analysis`, `--dump-kinds`, `--budget=` and the rest), and so are these `weavec-cc` flags, each now an `unknown WeaveC flag` error: `-fweavec-diagnose` and `-fno-weavec-diagnose`, `-fweavec-budget=`, `-fweavec-unit-budget=`, `-fweavec-dump-analysis` and `-fweavec-analysis-stats=`. The diagnostic controls (`-Wweavec`, `-Wweavec-<id>`, `-Wno-weavec-<id>`, `-Werror=weavec[-<id>]`, `-Wno-error=weavec[-<id>]`) are no longer WeaveC's: they reach Clang, which warns that each is an unknown warning option (`-Wunknown-warning-option`, an error under `-Werror`). `weavec.h` keeps only `WEAVEC_UNSAFE`; a program that uses one of its other annotation macros no longer compiles until it stops using it.

RFC 0035 removed these, also with no aliases (each is an unknown-flag error): `-fno-weavec-runtime` and `-fweavec-runtime` (use `-fweavec-checks=none`); `-f[no-]weavec-stack-objects` and `-f[no-]weavec-global-objects`; `-fweavec-require=` and `WEAVEC_REQUIRE_SAFE` with the require levels; `-fweavec-link=`, `-fweavec-link-budget=` and the `<object>.weavec` unit records; `-fweavec-ledger-format=` (SARIF); and `-fweavec-print-prelude`. Earlier RFCs removed the checked-mode options of RFCs 0018–0029, `-fweavec-exclusive-borrows`, `-fweavec-strict`, `-fweavec-analysis-cache=`, `-fweavec-analyze-headers` and `-fweavec-report-unannotated`.
