---
title: Integrate your build
description: Use weavec-cc as the C compiler with Make or CMake, understand what changes in the compiled program and its link, run the analysis from a compilation database, and add both to CI.
---

`weavec-cc` replaces the C compiler in an existing build. The `weavec` analysis tool runs beside the build, from its compilation database.

## Use the compiler driver with Make

```sh
make clean
make CC=weavec-cc
```

The Makefile must use `CC` for compilation and linking. If it invokes `ld` directly to link, route linking through `weavec-cc` so that it can add the runtime. To collect enforcement ledgers from a parallel build, name a directory:

```sh
make -j8 CC=weavec-cc CFLAGS="-O2 -fweavec-ledger=build/ledger/"
```

Each compile writes `<object>.ledger.json` into the directory.

## Use the compiler driver with CMake

Use a fresh build directory so CMake detects the new compiler:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc
cmake --build build-weavec
```

The driver forwards ordinary compiler options to Clang. WeaveC options use the `-fweavec-*` and `-W*weavec*` forms listed in the [command-line reference](/reference/cli/).

Configure scripts see Clang: `weavec-cc --version` prints Clang's version block first and `weavec-cc version …` after it, and a feature probe compiles as it would with Clang. On macOS a link with `-flto` uses the libLTO of the LLVM that WeaveC was built with.

## What changes in the compiled program

By default `weavec-cc` compiles with `-fweavec-checks=trap`. Each C file is compiled as Clang compiles it, with these additions:

- every load, store, atomic operation and memory intrinsic, and every memory argument of a C library call in the library table, gets a _guard_: a few inline instructions that read the runtime's shadow memory and call the runtime only when a byte they read is not zero. A guard is removed where a local rule proves it redundant;
- the locals a guard can reach are laid out in one frame with redzones, and the file's globals get redzones, which a constructor registers with the runtime;
- array indexes are checked against the bound the array's type declares (`-fsanitize=array-bounds`, reported through the runtime);
- locals are zero-initialised (`-ftrivial-auto-var-init=zero`);
- source locations are kept for the reports, without emitting debug information when you asked for none.

There is no change to pointer representation or the ABI. `-fweavec-checks=report` prints failed guards instead of trapping; `-fweavec-checks=none` compiles the same code as Clang would and links nothing. C++ and Objective-C sources are compiled as Clang compiles them.

Every link in the `trap`, `report` and `verify` modes adds two archives before the system libraries:

- `libweavec_rt.a`, the runtime: the shadow memory, the arena allocator's heap with its quarantine of freed blocks, the guards' slow paths, the checked library wrappers and the reports;
- `libweavec_alloc.a`, which defines `malloc`, `calloc`, `realloc`, `free` and the other standard allocation functions for the image, over the runtime's arena.

On Linux the executable's definitions serve every shared library in the process. On Darwin one runtime serves the process: the first copy the dynamic loader finds owns the arena and the shadow, and the runtimes of other images built by `weavec-cc` (shared libraries, plug-ins) forward to it. Its malloc zone becomes the process's default zone, so the C library's own allocations (`strdup`, `getline`, `asprintf` and the like) are tracked heap blocks too.

Link through `weavec-cc`; a link by another driver or by `ld` directly fails with undefined `__weavec_rt_` symbols. The build compiles without guards and links without the runtime when the command line has a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak`, `kernel-address`), `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, or a target other than 64-bit Darwin or Linux. The link says so:

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): memory accesses are not guarded
```

A program that defines `malloc`, `calloc`, `realloc` or `free` itself keeps its allocator. The runtime is still linked and the guards stay, but the heap is untracked:

```text
weavec-cc: note: 'alloc.o' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated
```

Objects compiled by `weavec-cc` and objects from another compiler link together. The other objects' code is not guarded; their calls into guarded code, and the memory they allocate through `malloc`, are covered as usual. The [command-line reference](/reference/cli/#the-runtime) lists the environment variables, and [safety guarantees](/reference/guarantees/) says what a guard promises.

## Analyze a compilation database

For CMake projects using Ninja or Makefiles:

```sh
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
weavec -p build src/main.c
weavec --whole-program -p build
```

`compile_commands.json` supplies each source file's compilation flags. The whole-program command selects all sources in the database when no source is named. Keep the database current when the build configuration changes. `weavec` builds nothing; it prints the analysis's diagnostics and a summary line per file. To get the same findings during a `weavec-cc` build, as warnings, add `-fweavec-diagnose`.

## Add checks to CI

Build and test with `weavec-cc` as the compiler:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc \
  -DCMAKE_C_FLAGS="-fweavec-ledger=$PWD/build-weavec/ledger/"
cmake --build build-weavec
ctest --test-dir build-weavec
```

Let a nonzero exit status fail the job: an error from the build, or a trap in a test. If tests capture standard error, set `WEAVEC_RT_REPORT_LOG=<path>` so the reports land in a file you can upload. Run `weavec --whole-program -p build` as a separate step to report what the analysis finds; its errors fail the step. Tests run slower under the guarded build than under a plain one; budget for it.
