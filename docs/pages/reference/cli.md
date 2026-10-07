---
title: Command-line reference
description: weavec-cc and weavec options for guard modes, zero-initialisation, the runtime and its environment variables, the enforcement ledger, the advisory analysis, budgets and diagnostic controls.
---

WeaveC has two tools. `weavec-cc` is Clang's compiler driver with WeaveC inside: it guards every memory access of the C code it compiles and links the runtime that the guards ask. `weavec` is a libTooling analysis tool: it runs the ownership and lifetime analysis and prints its diagnostics without producing objects. The options below are specified by [RFC 0035](/rfcs/0035-guard-by-default/) §7 and §8; `weavec-cc --help-weavec` and `weavec --help` list the ones your build accepts.

## Invocation

```sh
weavec-cc [clang options] [weavec-cc options] source.c -o program
weavec [options] source.c -- [clang options]
weavec [options] -p build [source.c ...]
weavec --whole-program [options] -p build
weavec --whole-program [options] a.c b.c -- [clang options]
```

`weavec-cc` accepts every Clang driver option. For `weavec`, place its options **before** `--` and the language standard, include paths, defines and target flags **after** it when you are not using a compilation database. With `-p build` and no source named, `--whole-program` analyses every source in `build/compile_commands.json`.

## weavec-cc

| Flag                                                       | Default                          | Meaning                                                                                                                                                                                                                                                             |
| ---------------------------------------------------------- | -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-fweavec` / `-fno-weavec`                                 | on                               | Guard memory accesses. Off compiles as plain Clang.                                                                                                                                                                                                                 |
| `-fweavec-checks=trap\|report\|verify\|none`               | `trap`                           | What a failed guard does; see [check modes](#check-modes).                                                                                                                                                                                                          |
| `-fweavec-zero-init` / `-fno-weavec-zero-init`             | on unless checks are `none`      | Zero-initialise locals of up to 4 KiB (`-ftrivial-auto-var-init=zero`; a `-ftrivial-auto-var-init-max-size` on the command line sets another limit). The runtime's allocator zero-fills every block regardless.                                                     |
| `-fweavec-ledger=<path>`                                   | unset                            | Write the [enforcement ledger](#ledger-and-summary-line). A value ending in `/`, or naming an existing directory, receives `<object>.ledger.json` per unit; a file receives one ledger, and naming one for an invocation that compiles several C files is an error. |
| `-fweavec-summary` / `-fno-weavec-summary`                 | off, on when a ledger is written | Print the unit's summary line on stderr.                                                                                                                                                                                                                            |
| `-fweavec-diagnose` / `-fno-weavec-diagnose`               | off                              | Also run the [advisory analysis](#the-advisory-analysis) on each unit and print its findings as warnings.                                                                                                                                                           |
| `-fweavec-budget=<n>`, `-fweavec-unit-budget=<n>`          | see [budgets](#budgets)          | The analysis's work budgets per function and per unit, under `-fweavec-diagnose`.                                                                                                                                                                                   |
| `-fweavec-dump-analysis`, `-fweavec-analysis-stats=<path>` | off                              | Debugging output of the analysis: the inferred facts (unstable format), and work statistics as JSON.                                                                                                                                                                |
| `--help-weavec`                                            |                                  | List the WeaveC flags and exit.                                                                                                                                                                                                                                     |

An unknown `-fweavec-*` flag is an error. Every other flag is Clang's. When WeaveC is on, the driver defines `__WEAVEC__=1` and puts the directory of `weavec.h` on the system include path. `weavec-cc --version` prints Clang's version block first, then `weavec-cc version <version> (<revision>)`, so a configure script that reads the first line sees Clang. On macOS a link with `-flto` uses the libLTO of the LLVM that WeaveC was built with.

C++, Objective-C, OpenCL and CUDA sources are compiled as Clang compiles them, without guards; so is everything under `-fno-weavec` or `-fweavec-checks=none`.

## Check modes

| `-fweavec-checks=` | A failed guard or index check                      | Guards removed by a local rule | Zero-init default | Runtime    | Guarantee                     |
| ------------------ | -------------------------------------------------- | ------------------------------ | ----------------- | ---------- | ----------------------------- |
| `trap` (default)   | prints a report and traps                          | removed                        | on                | linked     | yes                           |
| `report`           | prints a report once per site; the program goes on | removed                        | on                | linked     | only with `WEAVEC_RT_ABORT=1` |
| `verify`           | prints a report and traps                          | kept as monitors               | on                | linked     | yes                           |
| `none`             | nothing is guarded                                 | n/a                            | off               | not linked | no                            |

A trap ends the program with `SIGTRAP`, or `SIGILL` on x86-64. Before it traps, the runtime unblocks both signals in the failing thread and resets a disposition the program set to ignore them, so the program ends even with the signals blocked; a handler the program installed still runs. The report is one line on stderr, two for the heap:

```text
weavec: heap-buffer-overflow at vec.c:9:43: read of 4 bytes at 0xbd80000010
weavec: 0xbd80000010 is 0 bytes after the 16-byte heap object at 0xbd80000000
```

The kinds are `heap-buffer-overflow`, `heap-use-after-free`, `stack-buffer-overflow`, `dynamic-stack-buffer-overflow` (an `alloca` or a variable-length array), `stack-use-after-scope`, `global-buffer-overflow`, `null-dereference`, `unterminated-string` (a string argument of a library call with no terminator in its object), `overlapping-copy`, and `index-out-of-bounds` (`weavec: index-out-of-bounds at lookup.c:8:12: index -1`). An invalid `free` is stopped by the allocator, which knows no source location: `weavec: invalid release of 0x…: the block was already released` (or `not the start of its block`, `no block is allocated there`).

In report mode each failing site (kind, file, line and column) prints once and the program continues, with no guarantee; an invalid release still stops it, since the allocator cannot go on from one. Verify mode is the monitor of the removal rules: every guard a local rule removed is emitted too, and its failure prints `weavec: weavec.proven: <kind> at …` before the trap, which means WeaveC removed a guard it needed. Verify mode does not version loops.

## The runtime

Every link in the `trap`, `report` and `verify` modes adds `libweavec_alloc.a` (forced in with `-u malloc`) and `libweavec_rt.a` before the first library on the line, and `-lpthread` on Linux. The first defines `malloc`, `calloc`, `realloc`, `free` and the other standard allocation functions for the image, over the runtime's arena; the second holds the shadow memory, the guards' slow paths, the checked library wrappers and the reports. Link through `weavec-cc` so that it can add them; a link by another driver fails with undefined `__weavec_rt_` symbols. On Darwin one runtime serves the process, and the C library's own allocations (`strdup`, `getline`, `asprintf`) come from its arena too. [Safety guarantees](/reference/guarantees/) states what the guards promise.

`weavec-cc` compiles without guards and links without the runtime, as if `-fweavec-checks=none` had been given, when the command line has `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak` or `kernel-address`), or a target other than 64-bit Darwin or Linux. A link prints one of:

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): memory accesses are not guarded
weavec-cc: note: building without the WeaveC runtime (-nostdlib links no C library): memory accesses are not guarded
weavec-cc: note: building without the WeaveC runtime (-ffreestanding has no hosted C library): memory accesses are not guarded
weavec-cc: note: building without the WeaveC runtime (the runtime supports 64-bit Darwin and Linux targets): memory accesses are not guarded
```

When a link input, an object or a member of an archive, defines the allocator itself (`malloc`, `calloc`, `realloc` or `free`), `libweavec_alloc.a` is left off the link and the program keeps its allocator. The guards stay, but the heap is untracked:

```text
weavec-cc: note: 'alloc.o' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated
```

A program's own definition of another allocation function (`posix_memalign`, `reallocarray` and the like) over `malloc` replaces the runtime's and keeps the arena. A `free` that reaches the runtime's allocator from code another compiler built is still validated.

| Environment variable           | Meaning                                                                                                                                                                                              |
| ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `WEAVEC_RT_QUARANTINE=<bytes>` | How many bytes of released heap blocks are held before the oldest are reused. Default 16 MiB (16777216); `0` reuses blocks at once.                                                                  |
| `WEAVEC_RT_ABORT=1`            | In report mode, trap at the first failure as trap mode does.                                                                                                                                         |
| `WEAVEC_RT_REPORT_LOG=<path>`  | Append every report line to this file instead of printing it on stderr, so a test that captures stderr sees only the program's own output. Ignored by set-user-ID and set-group-ID programs.         |
| `WEAVEC_RT_STATS=1`            | At exit, print the runtime's counters on stderr, one per line, as `weavec: runtime: <n> <what>`. On Darwin only the runtime that owns the process prints, and its counters count every image's work. |

The counters of `WEAVEC_RT_STATS=1` are allocations, releases, recycled slots, huge blocks, slow guards (the guards whose inline check sent them to the runtime), range guards, string guards and stack unpoisons (the clearing of the stack's shadow before a call that does not return). They are not synchronised between threads.

## Ledger and summary line

The enforcement ledger records what the guard passes decided for each access of a unit. It is JSON, `weavec-ledger` version 3:

```json
{
  "schema": "weavec-ledger",
  "version": 3,
  "producer": { "name": "weavec-cc", "version": "0.15.0" },
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
|             | `optimized`    | The optimiser removed every copy of the access.                                          |
| `unguarded` | `unsafe`       | It lies in a `WEAVEC_UNSAFE` region or a function excluded from AddressSanitizer.        |

The rows are collected when the guards are inserted, before the optimiser; an access to a local or a global at a constant offset inside it gets neither a guard nor a row. A loop whose whole range was checked once still counts its accesses as `guarded`. `-fweavec-summary` prints the unit's counts:

```text
weavec: lookup.c: 3 accesses: 1 proven, 2 guarded, 0 unguarded
```

In a Makefile, pass a directory so parallel compiles write separate files:

```sh
make CC=weavec-cc CFLAGS="-O2 -fweavec-ledger=build/ledger/"
```

Every ledger is written to a temporary file and renamed into place. A link writes no ledger.

## The advisory analysis

The analysis infers ownership, borrows, lifetimes, nullness and extents, and reports what it finds. It never changes the code `weavec-cc` generates. It runs in two places:

- **`weavec`**, which builds nothing: a definite finding (one that holds on every path, confirmed on a feasible path) is an error and the tool exits with status 1; a possible one is a warning.
- **`weavec-cc -fweavec-diagnose`**, per unit as it compiles: every finding is a warning, so a correct program always builds. `-Werror=weavec` makes them errors. `leak` is off by default here; `-Wweavec-leak` turns it on.

`weavec` ends each unit with a summary line, and `--whole-program` adds one for the program, named after the unit that defines `main`:

```text
weavec: example.c: 11 sites: 6 proven, 3 not proven, 2 violations, 0 trusted; 2 errors, 1 warning
weavec: program app: 40 sites in 2 units: 31 proven, 9 not proven, 0 violations, 0 trusted; 0 errors, 0 warnings
```

Each memory operation (_site_) is counted once, by its worst safety facet: a violation, then not proven, trusted (it rests on a named assumption, such as a `WEAVEC_UNSAFE` region or a platform function), proven. Functions over budget are appended (`; 1 function over budget (<name>)`). The counts describe the analysis, not the build: a `weavec-cc` build guards the accesses the analysis proved as well as the others.

## weavec

| Option                              | Meaning                                                                                                                                               |
| ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--whole-program`                   | Analyse the given sources, or every source of the compilation database, as one program, so calls into other files use their definitions.              |
| `-p <dir>`                          | Read `compile_commands.json` from `<dir>` or a parent directory.                                                                                      |
| `--budget=<n>`                      | As `-fweavec-budget`.                                                                                                                                 |
| `--no-zero-init`                    | Model a build with `-fno-weavec-zero-init`.                                                                                                           |
| `--dump-analysis`                   | Print the inferred places, lifetimes, exit states and summaries of every analysed function (unstable format).                                         |
| `--dump-kinds`                      | Print each unit's pointer kinds, must-access requirements, store groups, field candidates and function-pointer slots instead of analysing (unstable). |
| `--analysis-stats=<path>`           | Write analysis work statistics as JSON.                                                                                                               |
| `--extra-arg`, `--extra-arg-before` | Add a compiler argument to every command.                                                                                                             |
| `-W…weavec…`                        | The [diagnostic controls](#diagnostic-controls), before `--`.                                                                                         |

A compile command with `-fdiagnostics-format=sarif` is an error in `weavec`.

## Budgets

Each analysis run counts its _work_: over its block transfers and joins, the size of each state it transfers or joins (its symbols and objects), so a run over large states costs what it costs ([RFC 0034](/rfcs/0034-fast-enforcement/) §7). A function over the budget stops early: the facets it has not decided are not proven, callers apply the unknown-callee effects to its arguments, and the summary line names it. The count is deterministic and independent of timing.

The default is 20,000,000 per run (Lua's `luaV_execute` takes about 10,000,000). A run is also over budget when the states it keeps at its blocks' entries hold more than 1,000,000 symbols and objects together, or when its function's graph has more than 100,000 blocks, which bounds the analysis's memory.

A second budget bounds a whole unit, which runs its functions many times (summary rounds, alias contexts, the final run): `-fweavec-unit-budget=<n>` work over every run, by default 400 per site of the unit and at least 20,000,000. Each run may take at most eight times its fair share of what is left, and at least 100,000, so a few large functions cannot spend what the others need. A function cut short this way is over budget like any other. `0` means unlimited. `weavec` uses the default unit budget.

## Diagnostic controls

These apply to `weavec` (before `--`) and to `weavec-cc -fweavec-diagnose`.

| Flag                       | Behaviour                                                                                                                                                                                               |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-Wno-weavec-<id>`         | Disable the warnings of `<id>`: always-warning ids and the possible (warning) findings of temporal ids.                                                                                                 |
| `-Wweavec-<id>`            | Enable the warnings of `<id>`. `allocation-failure` is off by default, and so is `leak` under `-fweavec-diagnose` (not in `weavec`); `-Wweavec-<id>`, `-Werror=weavec-<id>` or `-Wweavec` enables them. |
| `-Werror=weavec[-<id>]`    | Promote WeaveC warnings, or those of one id, to errors.                                                                                                                                                 |
| `-Wno-error=weavec[-<id>]` | Lower the errors of `<id>`, or of every id, to warnings.                                                                                                                                                |
| `-Wweavec`, `-Wno-weavec`  | Enable or disable every WeaveC warning.                                                                                                                                                                 |

Errors cannot be disabled, only lowered. A flag naming a removed identifier is an error. See the [diagnostic reference](/reference/diagnostics/) for the identifiers and [diagnostic controls](/reference/diagnostic-controls/) for details. Run-time reports are not diagnostics: these flags do not affect them.

## Removed options

RFC 0035 removed these, with no aliases (each is now an unknown-flag error): `-fno-weavec-runtime` and `-fweavec-runtime` (use `-fweavec-checks=none`); `-f[no-]weavec-stack-objects` and `-f[no-]weavec-global-objects`; `-fweavec-require=` and `WEAVEC_REQUIRE_SAFE` with the require levels; `-fweavec-link=`, `-fweavec-link-budget=` and the `<object>.weavec` unit records; `-fweavec-ledger-format=` (SARIF); `-fweavec-print-prelude`; and, in `weavec`, `--ledger`, `--ledger-format`, `--require`, `--no-runtime` and `--dump-record`. Earlier RFCs removed the checked-mode options of RFCs 0018–0029, `-fweavec-exclusive-borrows`, `-fweavec-strict`, `-fweavec-analysis-cache=`, `-fweavec-analyze-headers` and `-fweavec-report-unannotated`, with their `weavec` spellings.
