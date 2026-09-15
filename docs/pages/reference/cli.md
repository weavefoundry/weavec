---
title: Command-line reference
description: WeaveC analysis options, compiler-driver flags, warning controls, reports, and caching.
---

Use `weavec --help` for the installed analyzer's options. The forms below document the current source tree. `weavec-cc` accepts normal Clang driver options alongside WeaveC's flags.

## Invocation

```sh
weavec [options] source.c -- [clang options]
weavec [options] -p build [source.c ...]
weavec-cc [compiler options] source.c -o program
```

Place analyzer flags **before** `--`. Put language standards, include paths, defines, and target flags **after** it when you are not using a compilation database.

## Analysis and checked code

| Analyzer                          | Compiler driver                          | Purpose                                                           |
| --------------------------------- | ---------------------------------------- | ----------------------------------------------------------------- |
| `--whole-program`                 | Enabled at link time                     | Combine definitions across input translation units.               |
| `--checked`                       | `-fweavec-checked`                       | Require complete safety contracts for selected input definitions. |
| `--checked-function=name`         | `-fweavec-checked-function=name`         | Select a named definition; repeat to select several.              |
| `--checked-report=path`           | `-fweavec-checked-report=path`           | Write the checked safety report as JSON.                          |
| `--checked-report-format=compact` | `-fweavec-checked-report-format=compact` | Use compact reports; the default is `expanded`.                   |
| `--strict-externs`                | `-fweavec-strict`                        | Treat unresolved external calls as raw operations.                |
| `--exclusive-borrows`             | `-fweavec-exclusive-borrows`             | Enforce additional shared/mutable exclusivity rules.              |
| `--report-unannotated`            | `-fweavec-report-unannotated`            | Report missing interface annotations and inferred suggestions.    |
| `--analyze-headers`               | `-fweavec-analyze-headers`               | Include definitions found in headers.                             |
| `--dump-analysis`                 | `-fweavec-dump-analysis`                 | Print inferred facts and summaries; the debug format is unstable. |

Checked selection persists through compiler sidecars. A compile result deferred until link time is provisional. See [checked builds](/guides/checked-builds/).

## Reuse and statistics

| Analyzer                     | Compiler driver                     | Purpose                                                       |
| ---------------------------- | ----------------------------------- | ------------------------------------------------------------- |
| `--analysis-cache=directory` | `-fweavec-analysis-cache=directory` | Reuse validated translation-unit checkpoints. Off by default. |
| `--analysis-stats=path`      | `-fweavec-analysis-stats=path`      | Write analysis work statistics as JSON.                       |

The compiler's cache applies during link-time source replay. It does not skip ordinary code generation or authorize stale objects. See [cache and performance](/guides/incremental-analysis/).

## Compiler-only controls

| Flag               | Behavior                                             |
| ------------------ | ---------------------------------------------------- |
| `-fno-weavec`      | Compile without WeaveC analysis.                     |
| `-fweavec`         | Enable WeaveC analysis.                              |
| `-fno-weavec-link` | Skip the whole-program analysis step at link time.   |
| `-fweavec-link`    | Enable the whole-program analysis step at link time. |

## Diagnostic controls

| Flag                     | Behavior                                                    |
| ------------------------ | ----------------------------------------------------------- |
| `-Wno-weavec-<id>`       | Disable a diagnostic whose effective severity is a warning. |
| `-Wweavec-<id>`          | Enable the warning.                                         |
| `-Werror=weavec`         | Promote WeaveC warnings to errors.                          |
| `-Werror=weavec-<id>`    | Promote a specific diagnostic to an error.                  |
| `-Wno-error=weavec-<id>` | Lower a specific ordinary error to a warning.               |

Errors cannot be directly disabled. Checked failures remain failures independently of these severity controls. Use the [diagnostic reference](/reference/diagnostics/) for the available identifiers.
