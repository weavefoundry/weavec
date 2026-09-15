---
title: Troubleshooting
description: Resolve installation problems, missing compilation flags, stale sidecars, and incomplete checked results.
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

Analyze its source with the caller using `--whole-program`, or use the compiler driver through the final link. An unannotated declaration alone does not provide the body contract of an unavailable function.

For callbacks, ensure the actual targets are represented. A matching function-pointer type is not a substitute for a known target or accurate type contract.

## A checked build reports stale metadata

Rebuild both the object and its sidecar from the current source, headers, flags, and WeaveC version. Do not pair an older object with a new source-only analysis. Removing an optional analysis cache is safe; deleting required object metadata is not a way to establish a checked dependency.

## A clean ordinary run fails in checked mode

This can be expected. Ordinary diagnostics find supported violations; checked mode also rejects missing evidence. Read the reported obligation, then establish the missing fact or isolate the trusted boundary. See [safety guarantees](/reference/guarantees/).

## Report an issue

Include the WeaveC and LLVM versions, platform, exact command, a minimal source example, and the complete diagnostic or relevant report excerpt. Remove private source and environment details you do not intend to publish. Open a [bug report](https://github.com/weavefoundry/weavec/issues/new?template=bug_report.yml).
