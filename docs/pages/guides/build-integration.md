---
title: Integrate your build
description: Use WeaveC with CMake, Make, a compilation database, or the drop-in weavec-cc compiler driver.
---

Choose analysis alongside your build, or checking inside the compiler invocation.

## Analyze a compilation database

For CMake projects using Ninja or Makefiles:

```sh
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
weavec -p build src/main.c
weavec --whole-program -p build
```

`compile_commands.json` supplies each source file's compilation flags. The whole-program command selects all sources in the database when no explicit source list is given. Keep the database current when the build configuration changes.

## Use the compiler driver with Make

```sh
make clean
CC=weavec-cc make
```

The Makefile must honor `CC` for compilation and linking. If it assigns `CC` unconditionally, use `make CC=weavec-cc`. If it invokes `ld` directly at the link step, route linking through `weavec-cc` to perform whole-program checks.

## Use the compiler driver with CMake

Use a fresh build directory so CMake detects the new compiler:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc
cmake --build build-weavec
```

The driver forwards ordinary compiler options to Clang. WeaveC options use the `-fweavec-*` and `-W*weavec*` forms listed in the [command-line reference](/reference/cli/).

## Keep object sidecars

Compiling `buffer.c` produces both `buffer.o` and `buffer.o.weavec`. Keep the sidecar beside its object. At link time, WeaveC uses those summaries and validates recorded inputs before replaying analysis.

Rebuild objects when their source, headers, command, or sidecar format changes. A source-only reanalysis cannot prove that an older object matches new source. Ordinary archives do not yet transport member sidecars automatically; see [checked-build limits](/guides/checked-builds/).

## Add checks to CI

After installing WeaveC and preparing the build configuration, run the same command locally and in CI:

```sh
weavec --whole-program --checked-function=main \
  --checked-report=build/weavec-safety.json -p build
```

Let a nonzero exit status fail the job and retain the JSON report as a build artifact. Select the actual entry points or components you intend to check; not every existing codebase will immediately satisfy checked mode.
