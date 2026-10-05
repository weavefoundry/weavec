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

`compile_commands.json` supplies each source file's compilation flags. The whole-program command selects all sources in the database when no source is named. Keep the database current when the build configuration changes. `weavec` builds nothing and inserts no checks or guards; its ledger describes what a default `weavec-cc` build would do (`--no-runtime` describes a build with `-fno-weavec-runtime`).

## Use the compiler driver with Make

```sh
make clean
make CC=weavec-cc
```

The Makefile must use `CC` for compilation and linking. If it invokes `ld` directly at the link step, route linking through `weavec-cc` to get the link step and the runtime. To collect ledgers from a parallel build, name a directory:

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

Configure scripts see Clang: `weavec-cc --version` prints Clang's version block first and `weavec-cc version …` after it. On macOS a link with `-flto` uses the libLTO of the LLVM that WeaveC was built with.

## What changes in the compiled program

By default `weavec-cc` compiles with `-fweavec-checks=trap`: every null or bounds obligation it cannot prove, and can express as a check, gets a check that traps before the access. A spatial or temporal facet it leaves unresolved gets a _guard_ where the operation has a pointer to look up: a call into the runtime's object table that traps if the access leaves its object, or the object has been freed. It also zero-initialises locals and the standard allocation calls, and registers with the runtime the locals whose address escapes and the unit's globals. The checks and guards are plain C inserted before code generation, with no change to pointer representation or ABI. `-fweavec-checks=report` prints failed checks and guards instead of trapping; precompiled-header and module builds link a library of out-of-line check helpers. `-fweavec-checks=none` compiles the same code as Clang would and links nothing.

Every link in the `trap`, `report` and `verify` modes adds two archives before the system libraries:

- `libweavec_rt.a`, the runtime: the object table (heap blocks with their requested sizes, a quarantine of freed blocks, escaping stack locals, globals), the guards' entry points and report mode's printer;
- `libweavec_alloc.a`, which defines `malloc`, `calloc`, `realloc`, `free` and the other standard allocation functions for the image, over the runtime's arena.

On Linux the executable's definitions serve every shared library in the process. On Darwin one runtime serves the process: the first copy the dynamic loader finds owns the object table, and the runtimes of other images built by `weavec-cc` (shared libraries, plug-ins) forward to it and share its arena. Its malloc zone becomes the process's default zone, so the C library's own allocations (`strdup`, `getline`, `asprintf` and the like) are tracked heap blocks too.

Link through `weavec-cc` so that it can add them; a link by another driver or by `ld` directly fails with undefined `__weavec_rt_` symbols. The build goes on without the runtime, as if `-fno-weavec-runtime` had been given, when the command line has a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak`, `kernel-address`), `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, or a target other than 64-bit Darwin or Linux. The link says so:

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): guardable facets stay unresolved
```

A program that defines `malloc`, `calloc`, `realloc` or `free` itself keeps its allocator. The runtime is still linked, but the heap is untracked:

```text
weavec-cc: note: 'alloc.o' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated (RFC 0032)
```

Pass the same runtime flags to every compile and to the link. The link's flags decide whether the allocator is linked: objects compiled with the runtime and linked with `-fno-weavec-runtime` keep their guards, but their heap is untracked and the guards pass on it. The link says so, and the program ledger then records `"runtime": false` and counts the guarded facets as "guardable (not enforced)":

```text
weavec-cc: note: linking without the WeaveC runtime, but 'buffer.o' was compiled with it: the heap is untracked, its guards pass on it and releases are not validated (RFC 0032)
```

Units compiled with and without the runtime can be linked together; the program ledger then records `"runtime": "mixed"`. The [command-line reference](/reference/cli/#the-runtime) lists the flags and environment variables, and [safety guarantees](/reference/guarantees/#the-runtime) says what a guard promises and what the runtime costs.

## Keep object records

Compiling `buffer.c` produces both `buffer.o` and `buffer.o.weavec`, the unit's WeaveC record. Keep the record beside its object. At link time, WeaveC reads the records, checks each unit's declarations and requirements against the other units and composes the program ledger from their rows, without parsing or analysing any unit again; `-fweavec-link=analyze` also refines the temporal outcomes with the whole program in view (see [whole-program analysis](/guides/whole-program/#analyze-during-linking)). A record says whether its unit was compiled with the runtime.

A link input without a valid record is named in one `unanalyzed-input` warning per link: objects from another compiler, static archives, shared libraries outside the platform's own, and objects whose record is stale (an incompatible record format, or an object that changed after its record was written). Calls into those inputs are trusted, and the ledger lists them. Rebuild objects whose record is stale. Static archives and shared libraries do not carry records yet; an archive whose objects `weavec-cc` built is named in a note rather than the warning, since its code carries its own checks and guards.

## Add checks to CI

Build and test with `weavec-cc` as the compiler, and keep the ledger:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc \
  -DCMAKE_C_FLAGS="-fweavec-ledger=$PWD/build-weavec/ledger/ -fweavec-ledger-format=sarif"
cmake --build build-weavec
ctest --test-dir build-weavec
```

Let a nonzero exit status fail the job: an error from the build, or a trap in a test. A default link does not report bugs that span files; add `-fweavec-link=analyze` to the link flags (`CMAKE_EXE_LINKER_FLAGS`), or a `weavec --whole-program -p build-weavec` step, to report them. If tests capture standard error, run them in report mode with `WEAVEC_RT_REPORT_LOG=<path>`, which writes the reports to that file only. Upload the SARIF files to a code-scanning tool, or keep the JSON ledgers as artifacts. Add `-fweavec-require=guarded` to the components that should have no unresolved operation, or `-fweavec-require=checked` to those that should admit only proven or checked operations. Tests run slower under the runtime than under a plain build; budget for it, or build the hottest components with `-fno-weavec-runtime`.
