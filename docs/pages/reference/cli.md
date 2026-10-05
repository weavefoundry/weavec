---
title: Command-line reference
description: weavec-cc and weavec options for runtime checks, the runtime and its guards, zero-initialisation, require levels, the ledger, budgets and diagnostic controls.
---

WeaveC has two tools. `weavec-cc` is Clang's compiler driver with WeaveC inside: it analyses each file as it compiles it, inserts runtime checks and guards, checks the files' records against each other when it links, and links the runtime. `weavec` is a libTooling analysis tool: it runs the same analysis and writes the same ledger without producing objects. The options below are specified by [RFC 0030](/rfcs/0030-prove-or-trap/) §16, [RFC 0032](/rfcs/0032-runtime-enforcement/) §7.4 and [RFC 0033](/rfcs/0033-drop-in-by-default/); `weavec --help` and `weavec-cc --help-weavec` list the ones your build accepts.

## Invocation

```sh
weavec-cc [clang options] [weavec-cc options] source.c -o program
weavec [options] source.c -- [clang options]
weavec [options] -p build [source.c ...]
weavec --whole-program [options] -p build
```

`weavec-cc` accepts every Clang driver option. For `weavec`, place its options **before** `--` and the language standard, include paths, defines and target flags **after** it when you are not using a compilation database. With `-p build` and no source named, `--whole-program` analyses every source in `build/compile_commands.json`.

## weavec-cc

| Flag                                                       | Default                          | Meaning                                                                                                                                                                                                                |
| ---------------------------------------------------------- | -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-fweavec` / `-fno-weavec`                                 | on                               | Analyse, check and zero-initialise. Off compiles as plain Clang.                                                                                                                                                       |
| `-fweavec-checks=trap\|report\|verify\|none`               | `trap`                           | What unproven obligations become; see [modes](#check-modes).                                                                                                                                                           |
| `-fweavec-zero-init` / `-fno-weavec-zero-init`             | on unless checks are `none`      | Zero-initialise locals (`-ftrivial-auto-var-init=zero`) and the standard allocation calls.                                                                                                                             |
| `-fweavec-runtime` / `-fno-weavec-runtime`                 | on                               | Guard unresolved facets against the runtime's object table, register stack and global objects, and link the runtime's allocator. Nothing is emitted or linked when checks are `none`; see [the runtime](#the-runtime). |
| `-fweavec-stack-objects` / `-fno-weavec-stack-objects`     | on                               | Register this unit's locals whose address escapes with the runtime.                                                                                                                                                    |
| `-fweavec-global-objects` / `-fno-weavec-global-objects`   | on                               | Register this unit's globals with the runtime.                                                                                                                                                                         |
| `-fweavec-require=none\|guarded\|checked\|proven`          | `none`                           | Make unresolved facets (`guarded`), guarded facets too (`checked`), and checked facets too (`proven`), errors; see [require levels](#require-levels).                                                                  |
| `-fweavec-ledger=<path>`                                   | unset                            | Write the unit ledger when compiling and the program ledger when linking. A value ending in `/` or naming a directory writes one file per unit and per link.                                                           |
| `-fweavec-ledger-format=json\|sarif`                       | `json`                           | The ledger's format.                                                                                                                                                                                                   |
| `-fweavec-summary` / `-fno-weavec-summary`                 | off, on when a ledger is written | Print the one-line summary on stderr.                                                                                                                                                                                  |
| `-fweavec-budget=<n>`                                      | 50,000 (see [budgets](#budgets)) | Per-function limit on analysed CFG block transfers; `0` means unlimited.                                                                                                                                               |
| `-fweavec-unit-budget=<n>`                                 | 6 per site, at least 200,000     | Per-unit limit on block transfers over every analysis run of the unit; functions analysed after it is spent take the over-budget defaults. `0` means unlimited; see [budgets](#budgets).                               |
| `-fweavec-link=records\|analyze\|none`                     | `records`                        | What the link step does before linking; see [the link step](#the-link-step).                                                                                                                                           |
| `-fweavec-link-budget=<seconds>`                           | 120                              | Wall-clock budget of `-fweavec-link=analyze`; `0` means none.                                                                                                                                                          |
| `-fweavec-print-prelude`                                   | off                              | Print the check helpers for the current `-fweavec-checks` mode and exit.                                                                                                                                               |
| `-fweavec-dump-analysis`, `-fweavec-analysis-stats=<path>` | off                              | Debugging output: the inferred facts (unstable format), and work statistics as JSON.                                                                                                                                   |

An unknown `-fweavec-*` flag is an error. `weavec-cc --version` prints Clang's version block first, then `weavec-cc version <version> (<revision>)`, so a configure script that reads the first line sees Clang. On macOS a link with `-flto` uses the libLTO of the LLVM that WeaveC was built with.

## The link step

When `weavec-cc` links, it reads the WeaveC record (`<object>.weavec`) of each input before the link proper. `-fweavec-link=` selects what it does with them ([RFC 0033](/rfcs/0033-drop-in-by-default/) §7):

| Value               | What runs                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `records` (default) | The records alone; no unit is parsed or analysed again. Names the inputs without a record (`unanalyzed-input`), solves the program's function-pointer slots, checks declarations against definitions (`annotation-mismatch`), checks the requirements of exported functions at the calls other units make, and, with `-fweavec-ledger` or `-fweavec-summary`, composes the program ledger from the rows the records carry. |
| `analyze`           | `records`, then the units analysed again with the whole program in view, so that bugs that span units (a use after free whose `free` is in another unit) are reported, within `-fweavec-link-budget=<seconds>` of wall-clock time (default 120).                                                                                                                                                                           |
| `none`              | Nothing: the link proceeds as Clang's.                                                                                                                                                                                                                                                                                                                                                                                     |

Under `analyze`, a unit the budget cut, a unit that cannot be analysed again and a group of units that does not converge keep their compile-time results, and the link prints a note instead of failing:

```text
weavec-cc: note: the whole-program analysis of 'parser.c' stopped at its budget; their compile-time results stand
weavec-cc: note: the whole-program analysis of 'a.c', 'b.c' did not converge (its widened summaries are used); their compile-time results stand
```

Only a definite error fails a link, and `-Wno-error=weavec-<id>` lowers it. `weavec --whole-program` reports what `analyze` reports; there, too, a group that does not converge is a note.

## weavec

| Option                                     | Meaning                                                                                                                                               |
| ------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--whole-program`                          | Analyse the given sources, or every source of the compilation database, as one program.                                                               |
| `-p <dir>`                                 | Read `compile_commands.json` from `<dir>`.                                                                                                            |
| `--ledger=<path>`                          | Write the ledger; a directory (a value ending in `/`) receives one ledger per source.                                                                 |
| `--ledger-format=json\|sarif`              | The ledger's format (default `json`).                                                                                                                 |
| `--require=none\|guarded\|checked\|proven` | As `-fweavec-require`.                                                                                                                                |
| `--budget=<n>`                             | As `-fweavec-budget`.                                                                                                                                 |
| `--no-zero-init`                           | Model a build with `-fno-weavec-zero-init`.                                                                                                           |
| `--no-runtime`                             | Model a build with `-fno-weavec-runtime`: nothing is guarded.                                                                                         |
| `--dump-analysis`                          | Print the analysis engine's states and the summaries it inferred (unstable format).                                                                   |
| `--dump-kinds`                             | Print each unit's pointer kinds, must-access requirements, store groups, field candidates and function-pointer slots instead of analysing (unstable). |
| `--dump-record=<path>`                     | Print the unit record at `<path>` (an `<object>.weavec` that `weavec-cc` wrote) as JSON and exit; a stale record is an error that says why.           |
| `--analysis-stats=<path>`                  | Write analysis work statistics as JSON.                                                                                                               |
| `--extra-arg`, `--extra-arg-before`        | Add a compiler argument to every command.                                                                                                             |

The `weavec` ledger models a `weavec-cc` build with the default checks and the runtime. `weavec` always prints the summary line, and it inserts no checks or guards: its "checkable (not enforced)" and "guardable (not enforced)" facets are the ones `weavec-cc` would check and guard. Because it enforces nothing, it prints every possible temporal finding as a warning.

## Check modes

| `-fweavec-checks=` | Unproven facets                                                       | Proven facets                                                | Zero-init default | Runtime    | Guarantee                     |
| ------------------ | --------------------------------------------------------------------- | ------------------------------------------------------------ | ----------------- | ---------- | ----------------------------- |
| `trap` (default)   | checked or guarded; a failed check or guard traps                     | nothing                                                      | on                | linked     | yes                           |
| `report`           | checked or guarded; a failure prints a line and the program continues | nothing                                                      | on                | linked     | only with `WEAVEC_RT_ABORT=1` |
| `verify`           | checked or guarded; a failed check or guard traps                     | checked or guarded too where possible (`weavec.proven` trap) | on                | linked     | yes                           |
| `none`             | nothing                                                               | nothing                                                      | off               | not linked | no                            |

A trap ends the program with `SIGTRAP` or `SIGILL`, depending on the target. With the runtime, before it traps, a failed check or guard unblocks both signals in the failing thread and resets a disposition the program set to ignore them, so the program ends even with the signals blocked; a handler the program installed still runs. Report mode prints `weavec: runtime check failed: <template> at <file>:<line>:<column>` once per site and continues; with `WEAVEC_RT_ABORT=1` it aborts instead. The template is one of ten: the checks `nonnull`, `index`, `span`, `len`, `disjoint`, `assert` and `violation`, and the guards `object`, `live` and `release`. After a failed `release` guard in report mode the allocator ignores the release. Verify mode is the soundness monitor: a `weavec.proven` trap means the analysis proved something false. With the runtime it also monitors proven temporal facets, and proven spatial facets that have no static check, through guards.

Precompiled headers and modules keep working: the check helpers are then declared rather than injected, and `weavec-cc` links `libweavec_chk.a`, which defines them out of line; a function compiled from a precompiled header registers no stack objects. Comparing a function pointer with `malloc` sees the zero-initialising wrapper and becomes false; `-fno-weavec-zero-init` avoids it.

## The runtime

Every link in the `trap`, `report` and `verify` modes adds `libweavec_rt.a` and `libweavec_alloc.a` before the system libraries (and `-lpthread` on Linux). The first holds the object table and the guards' entry points; the second defines `malloc`, `calloc`, `realloc`, `free` and the other standard allocation functions for the image. With them, a spatial or temporal facet that the analysis leaves unresolved, at an operation with a pointer operand, becomes `guarded`: a guard looks the pointer up and traps if the access leaves its object or the object is dead. [Safety guarantees](/reference/guarantees/#the-runtime) states what a guard promises and what the runtime costs.

| Guard     | Placed at                                                                                                   | Fails when                                                                                                                                                                                                                                                                |
| --------- | ----------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `object`  | a dereference, a subscript, an argument of a call that needs a known number of bytes or a terminated string | the bytes accessed start in a dead tracked object, or in a live one they do not lie inside, or end in a tracked object they do not start in, or (for a subscript) leave a live object for untracked memory or reach forwards from one stack or global object into another |
| `live`    | a dereference, a subscript, a pointer argument of a library call                                            | the pointer points into a dead tracked object                                                                                                                                                                                                                             |
| `release` | the argument of `free` and of the other heap releasers                                                      | the pointer is not null, not the start of a live heap object, and points into a tracked object                                                                                                                                                                            |

A guard looks up the address it accesses (`p + i * step + offset` for a subscript), not the pointer it starts from, so a base formed outside a buffer and indexed back into it passes. Otherwise a guard passes on memory the runtime does not track.

Fortified calls (`__builtin___memcpy_chk` and the like, which `_FORTIFY_SOURCE` produces) are guarded as their plain forms are. A length the guard cannot repeat at the call, such as `memcpy(d, s, strlen(s))`, is checked where the call evaluates it. Each `%s` argument of a `printf`-family call with a literal format gets an `object` guard for its string, which also covers its liveness; a `%.Ns` argument reads at most N bytes and needs no terminator.

| Flag or variable                             | Meaning                                                                                                                                                                                               |
| -------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-fno-weavec-runtime`                        | When compiling: no guards and no registration; facets that would be guarded stay `unresolved`, and the ledger's `config.runtime` is `false`. When linking: `libweavec_alloc.a` is not linked.         |
| `-fno-weavec-stack-objects`                  | Do not register this unit's escaping locals. Guards pass on pointers into them.                                                                                                                       |
| `-fno-weavec-global-objects`                 | Do not register this unit's globals. Guards pass on pointers into them.                                                                                                                               |
| `-Wweavec-possible`, `-Wno-weavec-possible`  | Print possible temporal findings even where the facet is guarded. Off by default in enforcing builds with the runtime; otherwise they are always printed.                                             |
| `WEAVEC_RT_QUARANTINE=<bytes>` (environment) | The quarantine's budget: how many bytes of freed heap blocks are held before the oldest are reused. Default 64 MiB (67108864); `0` reuses blocks at once.                                             |
| `WEAVEC_RT_STATS=1` (environment)            | At exit, print the runtime's counters on stderr, one line each, as `weavec: runtime: <n> <what>`. On Darwin only the runtime that owns the process prints, and its counters count every image's work. |
| `WEAVEC_RT_ABORT=1` (environment)            | In report mode, abort at the first failed check or guard.                                                                                                                                             |
| `WEAVEC_RT_REPORT_LOG=<path>` (environment)  | In report mode, append every report line to this file instead of printing it on stderr, so a test that captures stderr sees only the program's own output.                                            |

The counters of `WEAVEC_RT_STATS=1` are allocations, releases, recycled slots, huge blocks, lookups (and of those heap, stack, global and untracked), range requests, ranges kept and stack objects entered. They are not synchronised between threads.

`weavec-cc` builds without the runtime, as `-fno-weavec-runtime` does, when neither `-fweavec-runtime` nor `-fno-weavec-runtime` was given and the command line has `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak` or `kernel-address`), or a target other than 64-bit Darwin or Linux. The link prints one of:

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): guardable facets stay unresolved
weavec-cc: note: building without the WeaveC runtime (-nostdlib links no C library): guardable facets stay unresolved
weavec-cc: note: building without the WeaveC runtime (-ffreestanding has no hosted C library): guardable facets stay unresolved
weavec-cc: note: building without the WeaveC runtime (the runtime supports 64-bit Darwin and Linux targets): guardable facets stay unresolved
```

When a link input defines `malloc`, `calloc`, `realloc` or `free`, `libweavec_alloc.a` is left off the link and the program keeps its allocator:

```text
weavec-cc: note: '<input>' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated (RFC 0032)
```

A link without the runtime whose inputs were compiled with it keeps their guards but leaves them no allocator to ask. The link prints this, and the program ledger records `"runtime": false` and counts those facets as "guardable (not enforced)":

```text
weavec-cc: note: linking without the WeaveC runtime, but '<input>' was compiled with it: the heap is untracked, its guards pass on it and releases are not validated (RFC 0032)
```

A program's own definition of another allocation function (`posix_memalign`, `reallocarray`, `malloc_usable_size` and the like) over `malloc` replaces the runtime's and keeps the arena. A `free` that reaches the runtime's allocator without a guard, for example from an object another compiler built, is still validated: an invalid one stops the program with `weavec: invalid release of <address>: <why>` on stderr.

## Require levels

| Level     | Error for each facet that is   | Diagnostic                                    |
| --------- | ------------------------------ | --------------------------------------------- |
| `none`    | nothing (the default)          |                                               |
| `guarded` | unresolved                     | `unresolved-operation`                        |
| `checked` | unresolved or guarded          | `unresolved-operation`                        |
| `proven`  | unresolved, guarded or checked | `unresolved-operation`, `unchecked-operation` |

Trusted facets are allowed at every level. An unresolved facet's message reads `<operation> is neither proven nor checkable: <reason phrase> [<reason>]`, a guarded facet's `<operation> is guarded at run time only: <reason phrase> [<reason>]`. Without the runtime nothing is guarded, so `guarded` and `checked` reject the same facets. `WEAVEC_REQUIRE_SAFE` before a function definition holds that function to `checked` whatever the command line says. A unit that fails its require level produces no object.

## Ledger and summary line

The ledger is JSON (`weavec-ledger`, version 2) or SARIF 2.1.0. It lists every function, every site and every facet with its outcome (`proven`, `checked`, `guarded`, `violation`, `unresolved` or `trusted`), the reason for each unresolved, guarded or trusted facet, the check or guard template, fix-it suggestions, the diagnostics, and a stable fingerprint per facet and per diagnostic that survives edits elsewhere in the file and function. At link it adds what the program relies on under each assumption (A1–A5) and the link inputs without WeaveC records. Output is deterministic.

Version 2 added, for the runtime: `config.runtime` (`true`, `false`, or `"mixed"` in a program ledger whose units disagree); a `guarded` count beside every other outcome count; `summary.guardedReasons`, the histogram of reasons over guarded facets, beside `summary.unresolvedReasons`; and `summary.unresolvedShare` and `summary.guardedShare`, the share of facets with that outcome per facet (`spatial`, `null`, `temporal`; `unresolvedShare` also has `spatialNull`). A guarded row keeps the reason the analysis gave:

```json
"spatial": { "outcome": "guarded", "reason": "unknown-extent", "check": { "template": "object" } }
```

In SARIF a guarded facet is a `note`-level result with `properties.outcome` set to `guarded`. A possible finding that the build does not print because its facet is guarded is not in the ledger's `diagnostics` either; the row keeps its reason.

The summary line is printed once per unit, and once per link:

```text
weavec: runtime-flags.c: 4 sites: 2 proven, 1 checked, 1 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings
```

Each site is counted once, by its worst facet: violation, then unresolved, guarded, checked, trusted, proven. The guarded count is always printed. With `-fweavec-checks=none`, and in `weavec`, "checked" reads "checkable (not enforced)" and "guarded" reads "guardable (not enforced)". With `-fno-weavec-runtime` the guarded count is `0 guardable (not enforced)` and those facets are counted as unresolved:

```text
weavec: runtime-flags.c: 4 sites: 2 proven, 1 checked, 0 guardable (not enforced), 1 unresolved, 0 trusted; 0 errors, 0 warnings
```

A non-zero violation count is inserted after the guarded count (`…, 1 checked, 2 guarded, 2 violations, 0 unresolved, …`), and over-budget functions are appended (`; 1 function over budget (<name>)`). The link's line names the program and its unit count, then the inputs without a WeaveC record, if any (`; 1 input without a WeaveC record (<name>)`), and the unverified A1 and A3 counts:

```text
weavec: program vec: 11 sites in 1 unit: 8 proven, 2 checked, 1 guarded, 0 unresolved, 0 trusted; 0 errors, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)
```

In a Makefile, pass a directory so parallel compiles write separate files:

```sh
make CC=weavec-cc CFLAGS="-O2 -fweavec-ledger=build/ledger/"
```

Every ledger is written to a temporary file and renamed into place. Naming one file that a single invocation would write twice (a compile and a link in one command) is an error.

## Budgets

Each function's analysis counts the CFG blocks it transfers. A function over the budget stops early: its null facets, and spatial facets whose exact extent needs no flow facts, fall back to runtime checks; its other facets are `unresolved(budget)`; callers apply the unknown-callee effects to its arguments; and the summary line and ledger name it. The count is deterministic and independent of timing.

The default is 50,000 block transfers, calibrated by the rule of RFC 0030 §5.5: the smallest multiple of 50,000 that is at least four times the 99.9th percentile of the per-function counts over the corpus (7,208 over 2,874 functions; the largest, Lua's `luaV_execute`, is 25,518), so no corpus function exceeds it.

A second budget bounds a whole unit, which runs its functions many times (summary rounds, alias contexts, the final run): `-fweavec-unit-budget=<n>` block transfers over every run, by default 6 per site of the unit and at least 200,000 ([RFC 0033](/rfcs/0033-drop-in-by-default/) §9). Each run may take at most eight times its fair share of what is left, and at least 500 transfers, so a few large functions cannot spend what the others need. A function cut short this way is over budget like any other. `0` means unlimited. `weavec` uses the default unit budget.

## Diagnostic controls

| Flag                                        | Behavior                                                                                                                                                                                                                                                                                                  |
| ------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-Wno-weavec-<id>`                          | Disable the warnings of `<id>`: always-warning ids and the possible (warning) findings of temporal ids.                                                                                                                                                                                                   |
| `-Wweavec-<id>`                             | Enable the warnings of `<id>`. `allocation-failure` is off by default, and so is `leak` in `weavec-cc` (not in `weavec`); `-Wweavec-<id>`, `-Werror=weavec-<id>` or `-Wweavec` enables them.                                                                                                              |
| `-Werror=weavec[-<id>]`                     | Promote WeaveC warnings, or those of one id, to errors.                                                                                                                                                                                                                                                   |
| `-Wno-error=weavec-<id>`                    | Lower the errors of `<id>` to warnings. A lowered temporal violation, or a violation of a callee's or library function's requirement, is guarded and traps only if it happens; a spatial or null violation keeps its check or an unconditional trap, as does every lowered violation without the runtime. |
| `-Wweavec`, `-Wno-weavec`                   | Enable or disable every WeaveC warning.                                                                                                                                                                                                                                                                   |
| `-Wweavec-possible`, `-Wno-weavec-possible` | Not an id but a switch: print, or do not print, possible temporal findings on facets the build guards; see [the runtime](#the-runtime).                                                                                                                                                                   |

Errors cannot be disabled, only lowered. A flag naming a removed identifier is an error. See the [diagnostic reference](/reference/diagnostics/) for the identifiers and [diagnostic controls](/reference/diagnostic-controls/) for details.

## Removed options

RFC 0030 removed these options from both tools, with no aliases: the checked-mode selection and report options of RFCs 0018–0029; `--exclusive-borrows` and `-fweavec-exclusive-borrows`; `--strict-externs` and `-fweavec-strict` (replaced by the sound defaults for unknown code and the require levels); `--analysis-cache` and `-fweavec-analysis-cache=`; `--analyze-headers` and `-fweavec-analyze-headers` (every emitted function, including `static inline` functions from your headers, is analysed); and `--report-unannotated` and `-fweavec-report-unannotated` (replaced by fix-its in the ledger). RFC 0033 replaced `-fweavec-link` and `-fno-weavec-link` with `-fweavec-link=records|analyze|none`, with no alias.
