---
title: Command-line reference
description: weavec-cc and weavec options for runtime checks, zero-initialisation, require levels, the ledger, budgets and diagnostic controls.
---

WeaveC has two tools. `weavec-cc` is Clang's compiler driver with WeaveC inside: it analyses each file as it compiles it, inserts runtime checks, and analyses the program again when it links. `weavec` is a libTooling analysis tool: it runs the same analysis and writes the same ledger without producing objects. The options below are specified by [RFC 0030](/rfcs/0030-prove-or-trap/) §16; `weavec --help` and `weavec-cc --help-weavec` list the ones your build accepts.

## Invocation

```sh
weavec-cc [clang options] [weavec-cc options] source.c -o program
weavec [options] source.c -- [clang options]
weavec [options] -p build [source.c ...]
weavec --whole-program [options] -p build
```

`weavec-cc` accepts every Clang driver option. For `weavec`, place its options **before** `--` and the language standard, include paths, defines and target flags **after** it when you are not using a compilation database. With `-p build` and no source named, `--whole-program` analyses every source in `build/compile_commands.json`.

## weavec-cc

| Flag                                                       | Default                           | Meaning                                                                                                                                                      |
| ---------------------------------------------------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `-fweavec` / `-fno-weavec`                                 | on                                | Analyse, check and zero-initialise. Off compiles as plain Clang.                                                                                             |
| `-fweavec-checks=trap\|report\|verify\|none`               | `trap`                            | What unproven spatial and null obligations become; see [modes](#check-modes).                                                                                |
| `-fweavec-zero-init` / `-fno-weavec-zero-init`             | on unless checks are `none`       | Zero-initialise locals (`-ftrivial-auto-var-init=zero`) and the standard allocation calls.                                                                   |
| `-fweavec-require=none\|checked\|proven`                   | `none`                            | Make unresolved facets (`checked`), or unresolved and checked facets (`proven`), errors; see [require levels](#require-levels).                              |
| `-fweavec-ledger=<path>`                                   | unset                             | Write the unit ledger when compiling and the program ledger when linking. A value ending in `/` or naming a directory writes one file per unit and per link. |
| `-fweavec-ledger-format=json\|sarif`                       | `json`                            | The ledger's format.                                                                                                                                         |
| `-fweavec-summary` / `-fno-weavec-summary`                 | off, on when a ledger is written  | Print the one-line summary on stderr.                                                                                                                        |
| `-fweavec-budget=<n>`                                      | 200,000 (see [budgets](#budgets)) | Per-function limit on analysed CFG block transfers; `0` means unlimited.                                                                                     |
| `-fweavec-link` / `-fno-weavec-link`                       | on                                | Run the whole-program step when linking.                                                                                                                     |
| `-fweavec-print-prelude`                                   | off                               | Print the check helpers for the current `-fweavec-checks` mode and exit.                                                                                     |
| `-fweavec-dump-analysis`, `-fweavec-analysis-stats=<path>` | off                               | Debugging output: the inferred facts (unstable format), and work statistics as JSON.                                                                         |

An unknown `-fweavec-*` flag is an error.

## weavec

| Option                              | Meaning                                                                                                               |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------- |
| `--whole-program`                   | Analyse the given sources, or every source of the compilation database, as one program.                               |
| `-p <dir>`                          | Read `compile_commands.json` from `<dir>`.                                                                            |
| `--ledger=<path>`                   | Write the ledger; a directory (a value ending in `/`) receives one ledger per source.                                 |
| `--ledger-format=json\|sarif`       | The ledger's format (default `json`).                                                                                 |
| `--require=none\|checked\|proven`   | As `-fweavec-require`.                                                                                                |
| `--budget=<n>`                      | As `-fweavec-budget`.                                                                                                 |
| `--no-zero-init`                    | Model a build with `-fno-weavec-zero-init`.                                                                           |
| `--dump-analysis`                   | Print inferred places, lifetimes, exit states and summaries (unstable format).                                        |
| `--dump-kinds`                      | Print each unit's pointer kinds, must-access requirements and function-pointer slots instead of analysing (unstable). |
| `--analysis-stats=<path>`           | Write analysis work statistics as JSON.                                                                               |
| `--extra-arg`, `--extra-arg-before` | Add a compiler argument to every command.                                                                             |

The `weavec` ledger models a `weavec-cc` build with the default checks. `weavec` always prints the summary line, and it inserts no checks: its "checkable (not enforced)" facets are the ones `weavec-cc` would check.

## Check modes

| `-fweavec-checks=` | Unproven spatial and null facets                    | Proven facets                                        | Zero-init default | Guarantee                     |
| ------------------ | --------------------------------------------------- | ---------------------------------------------------- | ----------------- | ----------------------------- |
| `trap` (default)   | checked; a failed check traps                       | nothing                                              | on                | yes                           |
| `report`           | checked; a failed check prints a line and continues | nothing                                              | on                | only with `WEAVEC_RT_ABORT=1` |
| `verify`           | checked; a failed check traps                       | checked too where expressible (`weavec.proven` trap) | on                | yes                           |
| `none`             | nothing                                             | nothing                                              | off               | no                            |

A trap ends the program with `SIGTRAP` or `SIGILL`, depending on the target. Report mode prints `weavec: runtime check failed: <template> at <file>:<line>:<column>` once per site, where the template is `nonnull`, `index`, `span`, `len`, `disjoint`, `assert` or `violation`, and links `libweavec_rt.a`; with `WEAVEC_RT_ABORT=1` it aborts instead. Verify mode is the soundness monitor: a `weavec.proven` trap means the analysis proved something false. Temporal facets are never checked at run time in any mode.

Precompiled headers and modules keep working: the check helpers are then declared rather than injected, and `weavec-cc` links `libweavec_chk.a`, which defines them out of line. Comparing a function pointer with `malloc` sees the zero-initialising wrapper and becomes false; `-fno-weavec-zero-init` avoids it.

## Require levels

| Level     | Error for each facet that is | Diagnostic                                    |
| --------- | ---------------------------- | --------------------------------------------- |
| `none`    | nothing (the default)        |                                               |
| `checked` | unresolved                   | `unresolved-operation`                        |
| `proven`  | unresolved or checked        | `unresolved-operation`, `unchecked-operation` |

Trusted facets are allowed at every level. `WEAVEC_REQUIRE_SAFE` before a function definition holds that function to `checked` whatever the command line says. A unit that fails its require level produces no object.

## Ledger and summary line

The ledger is JSON (`weavec-ledger`, version 1) or SARIF 2.1.0. It lists every function, every site and every facet with its outcome, the reason for each unresolved or trusted facet, fix-it suggestions, the diagnostics, and a stable fingerprint per row that survives edits elsewhere in the file. At link it adds what the program relies on under each assumption (A1–A5) and the link inputs without WeaveC records. Output is deterministic.

The summary line is printed once per unit, and once per link:

```text
weavec: cJSON.c: 4,210 sites: 3,050 proven, 980 checked, 150 unresolved, 30 trusted; 0 errors, 2 warnings
```

With `-fweavec-checks=none`, and in `weavec`, "checked" reads "checkable (not enforced)". Over-budget functions are appended (`; 1 function over budget (<name>)`). In a Makefile, pass a directory so parallel compiles write separate files:

```sh
make CC=weavec-cc CFLAGS="-O2 -fweavec-ledger=build/ledger/"
```

Every ledger is written to a temporary file and renamed into place. Naming one file that a single invocation would write twice (a compile and a link in one command) is an error.

## Budgets

Each function's analysis counts the CFG blocks it transfers. A function over the budget stops early: its null facets, and spatial facets whose exact extent needs no flow facts, fall back to runtime checks; its other facets are `unresolved(budget)`; callers apply the unknown-callee effects to its arguments; and the summary line and ledger name it. The count is deterministic and independent of timing.

The default is 200,000 block transfers, the initial value of RFC 0030 §5.5. `TODO(S8)`: record the calibrated default here once the corpus calibration of §5.5 has run.

## Diagnostic controls

| Flag                      | Behavior                                                                                                |
| ------------------------- | ------------------------------------------------------------------------------------------------------- |
| `-Wno-weavec-<id>`        | Disable the warnings of `<id>`: always-warning ids and the possible (warning) findings of temporal ids. |
| `-Wweavec-<id>`           | Enable the warnings of `<id>`; `-Wweavec-allocation-failure` enables the one id that is off by default. |
| `-Werror=weavec[-<id>]`   | Promote WeaveC warnings, or those of one id, to errors.                                                 |
| `-Wno-error=weavec-<id>`  | Lower the errors of `<id>` to warnings. A lowered violation is still emitted behind a trapping check.   |
| `-Wweavec`, `-Wno-weavec` | Enable or disable every WeaveC warning.                                                                 |

Errors cannot be disabled, only lowered. A flag naming a removed identifier is an error. See the [diagnostic reference](/reference/diagnostics/) for the identifiers and [diagnostic controls](/reference/diagnostic-controls/) for details.

## Removed options

RFC 0030 removed these options from both tools, with no aliases: the checked-mode selection and report options of RFCs 0018–0029; `--exclusive-borrows` and `-fweavec-exclusive-borrows`; `--strict-externs` and `-fweavec-strict` (replaced by the sound defaults for unknown code and the require levels); `--analysis-cache` and `-fweavec-analysis-cache=`; `--analyze-headers` and `-fweavec-analyze-headers` (every emitted function, including `static inline` functions from your headers, is analysed); and `--report-unannotated` and `-fweavec-report-unannotated` (replaced by fix-its in the ledger).
