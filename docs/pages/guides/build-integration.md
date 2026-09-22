---
title: Integrate your build
description: Use WeaveC with CMake, Make, a compilation database, or the drop-in weavec-cc compiler driver, and keep its ledger in CI.
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

`compile_commands.json` supplies each source file's compilation flags. The whole-program command selects all sources in the database when no source is named. Keep the database current when the build configuration changes. `weavec` builds nothing and inserts no checks; its ledger describes what a `weavec-cc` build would do.

## Use the compiler driver with Make

```sh
make clean
make CC=weavec-cc
```

The Makefile must use `CC` for compilation and linking. If it invokes `ld` directly at the link step, route linking through `weavec-cc` to get the whole-program step. To collect ledgers from a parallel build, name a directory:

```sh
make -j8 CC=weavec-cc CFLAGS="-O2 -fweavec-ledger=build/ledger/"
```

Each compile writes `<object>.ledger.json` and each link `<output>.ledger.json` into the directory.

## Use the compiler driver with CMake

Use a fresh build directory so CMake detects the new compiler:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc
cmake --build build-weavec
```

The driver forwards ordinary compiler options to Clang. WeaveC options use the `-fweavec-*` and `-W*weavec*` forms listed in the [command-line reference](/reference/cli/).

## What changes in the compiled program

By default `weavec-cc` compiles with `-fweavec-checks=trap`: every null or bounds obligation it cannot prove, and can express as a check, gets a check that traps before the access. It also zero-initialises locals and the standard allocation calls. The checks are plain C inserted before code generation: no ABI change, and no runtime library in the default mode. `-fweavec-checks=report` links WeaveC's small runtime library to print failed checks instead of trapping; precompiled-header and module builds link a library of out-of-line check helpers. `-fweavec-checks=none` compiles the same code as Clang would.

## Keep object records

Compiling `buffer.c` produces both `buffer.o` and `buffer.o.weavec`, the unit's WeaveC record. Keep the record beside its object. At link time, WeaveC reads the records, checks each unit's declarations and requirements against the other units, and refines the temporal outcomes with the whole program in view.

A link input without a valid record is named in one `unanalyzed-input` warning per link: objects from another compiler, static archives, shared libraries outside the platform's own, and objects whose record is stale (an incompatible record format, or an object that changed after its record was written). Calls into those inputs are trusted, and the ledger lists them. Rebuild objects whose record is stale. Static archives and shared libraries do not carry records yet.

## Add checks to CI

Build and test with `weavec-cc` as the compiler, and keep the ledger:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc \
  -DCMAKE_C_FLAGS="-fweavec-ledger=$PWD/build-weavec/ledger/ -fweavec-ledger-format=sarif"
cmake --build build-weavec
ctest --test-dir build-weavec
```

Let a nonzero exit status fail the job: an error from the build, or a trap in a test. Upload the SARIF files to a code-scanning tool, or keep the JSON ledgers as artifacts. Add `-fweavec-require=checked` to the components that should admit only proven or checked operations.
