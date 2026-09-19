---
title: Troubleshooting
description: Resolve installation problems, missing compilation flags, runtime traps, unresolved ledger rows, and link inputs without WeaveC records.
---

## CMake cannot find LLVM or Clang

Install the development libraries, not just a compiler executable. Set `WEAVEC_LLVM_PREFIX` to the LLVM installation prefix, or supply `CMAKE_PREFIX_PATH`, `LLVM_DIR`, and `Clang_DIR`. On macOS, use Homebrew LLVM; Xcode does not ship the required CMake packages.

Use a fresh build directory if a previous configuration selected a different LLVM installation. See the [developer toolchain guide](/contributing/development/#toolchain).

## FileCheck or lit is missing

Tests require `lit` plus LLVM's `FileCheck`, `not`, and `count` utilities. Make sure the chosen LLVM tool directory is on PATH and use a matching `lit` release. Building LLVM from source requires its utilities to be installed.

## The installed compiler cannot locate Clang resources

Keep the LLVM installation used at configure time available. The driver records its resource directory and Clang executable. `WEAVEC_RESOURCE_DIR` and `WEAVEC_CLANG` can override those locations when using a compatible installation. See [build targets](/contributing/development/#building).

## Headers or macros are missing during analysis

Use a current `compile_commands.json` with `-p build`, or pass your actual include directories and defines after `--`:

```sh
weavec src/main.c -- -std=c17 -Iinclude -DPROJECT_FEATURE=1
```

Resolve Clang parse errors before interpreting analysis results.

## A helper's behavior is not visible

Analyze its source with the caller using `--whole-program`, or use the compiler driver through the final link. An unannotated declaration alone does not describe an unavailable function: WeaveC assumes it may free, keep or replace its pointer arguments, and the ledger lists the affected operations as `unresolved(unknown-callee)` with a suggested annotation.

For callbacks, ensure the actual targets are stored somewhere the program can see. A function-pointer type alone does not say which function a call reaches.

## A program built with weavec-cc traps

A trap (`SIGTRAP` or `SIGILL`) means a runtime check failed: a null dereference or an out-of-bounds access was about to happen. Rebuild with `-fweavec-checks=report` and rerun; each failed check prints `weavec: runtime check failed: <kind> at <file>:<line>:<column>` and the program continues, so one run shows every failing site. The ledger row at that line says what was checked.

Usually the trap is a real bug. Sometimes the code relies on undefined behavior that happens to work, such as reading one element past an array; fix it, or move the operation into a narrow `WEAVEC_UNSAFE` region. Declared extents are enforced too: a call that passes less than a `WEAVEC_COUNTED_BY(n)` parameter promises can trap at the call.

## The ledger has many unresolved rows

Read `summary.unresolvedReasons` in the ledger. `unknown-callee` rows go away when the callee's definition is linked in or its declaration states its ownership; `unknown-extent` rows need a declared extent (`WEAVEC_COUNTED_BY`, `WEAVEC_ENDED_BY`, `WEAVEC_STRING`); `budget` rows name a function that exceeded the analysis budget (`-fweavec-budget`). See [adopt WeaveC incrementally](/guides/adoption/).

## The link warns about an unanalyzed input

`unanalyzed-input` names the link inputs that have no valid WeaveC record: objects from another compiler, static archives, shared libraries, and objects whose record is stale. Calls into them are trusted. Rebuild the objects with `weavec-cc`; keep each `.o.weavec` record beside its object. Archives and shared libraries do not carry records yet.

## A require level rejects the build

`-fweavec-require=checked` makes every unresolved operation an `unresolved-operation` error, and `-fweavec-require=proven` also makes every runtime-checked operation an `unchecked-operation` error. The message names the reason. Resolve it as above or lower the level for that component. A spatial or null operation that is correct for reasons WeaveC cannot see can go in a reviewed `WEAVEC_UNSAFE` region: its facets become trusted, which every level allows.

## Report an issue

Include the WeaveC and LLVM versions, platform, exact command, a minimal source example, and the complete diagnostic or relevant report excerpt. Remove private source and environment details you do not intend to publish. Open a [bug report](https://github.com/weavefoundry/weavec/issues/new?template=bug_report.yml).
