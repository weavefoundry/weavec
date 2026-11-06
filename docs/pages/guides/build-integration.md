---
title: Integrate your build
description: Use weavec-cc as the C compiler with Make, CMake or a configure script, understand what changes in the compiled program and its link, and add the guarded build to CI.
---

`weavec-cc` replaces the C compiler in an existing build. Nothing else in the build changes.

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

The driver forwards ordinary compiler options to Clang. WeaveC's own options use the `-fweavec-*` form listed in the [command-line reference](/reference/cli/); `weavec-cc --help-weavec` prints them.

Configure scripts see Clang: `weavec-cc --version` prints Clang's version block first and `weavec-cc version …` after it, and a feature probe compiles as it would with Clang. On macOS a link with `-flto` uses the libLTO of the LLVM that WeaveC was built with.

## What changes in the compiled program

By default `weavec-cc` compiles with `-fweavec-checks=trap`. Each C file is compiled as Clang compiles it, with these additions:

- every load, store, atomic operation and memory intrinsic, and every memory argument of a C library call in the library table, gets a _guard_: a few inline instructions that read the runtime's shadow memory and call the runtime only when a byte they read is not zero. A guard is removed only where a local rule proves it redundant;
- the locals a guard can reach, or whose address escapes, are laid out in one frame with redzones between them, and locals whose lifetimes never overlap share space as Clang's own frames do. The file's globals get redzones, which a constructor registers with the runtime;
- array indexes are checked against the bound the array's type declares (`-fsanitize=array-bounds`, reported through the runtime), and a fortified library call (`__strcpy_chk` and the like) is checked against the object size it carries. On glibc targets an optimizing compile that does not set `_FORTIFY_SOURCE` gets `-D_FORTIFY_SOURCE=3`;
- the file is parsed as AddressSanitizer would parse it: `__SANITIZE_ADDRESS__` is defined and `__has_feature(address_sanitizer)` is true, so code written to be ASan-clean takes the same paths. `-fno-weavec-asan-compat` turns this off;
- locals are zero-initialized (`-ftrivial-auto-var-init=zero`; `-fno-weavec-zero-init` turns it off), and `__WEAVEC__` is defined;
- source locations are kept for the reports, without emitting debug information when you asked for none.

There is no change to pointer representation or the ABI. `-fweavec-checks=report` prints failed guards instead of trapping; `-fweavec-checks=none` (or `-fno-weavec`) compiles the same code as Clang would and links nothing. C++ and Objective-C sources are compiled as Clang compiles them.

Every link in the `trap`, `report` and `verify` modes, of a program, a shared library or a plug-in, adds the runtime's shared library before the system libraries, with an rpath to the toolchain's `lib/weavec`: `libweavec_rt.so.<abi>` on Linux, `libweavec_rt.<abi>.dylib` on Darwin. It holds the shadow memory, the arena allocator with `malloc`, `calloc`, `realloc`, `free` and the other standard allocation functions, the quarantine of released blocks and the sweep that recycles them, the guards' slow paths, the checked library wrappers, AddressSanitizer's poisoning interface and the reports. A `-static` Linux program takes `libweavec_rt.a` instead.

Every image `weavec-cc` linked depends on the library, so the loader loads it once and one runtime serves the process, whatever the images' version scripts, `-Bsymbolic`, export lists or link order: a block one image allocates another may release, and every image's globals have their redzones in the process's one shadow. On Linux its `malloc` precedes the C library's in the search order and serves every image; on Darwin its malloc zone becomes the process's default zone. Either way the C library's own allocations (`strdup`, `getline`, `asprintf` and the like) are tracked heap blocks too.

A guarded program needs the library where it runs. Built and run on one machine, the rpath finds it in the toolchain. To ship a program, install `libweavec_rt` beside it and link with `-fweavec-rpath='$ORIGIN'` on Linux or `-fweavec-rpath=@loader_path` on Darwin (or `-fno-weavec-rpath` and an rpath of your own). A guarded plug-in loaded by a program `weavec-cc` did not link (a Python extension module) keeps its frames and globals checked; on Linux its heap is the C library's, unless the program runs with `LD_PRELOAD=<toolchain>/lib/weavec/libweavec_rt.so.<abi>`, and the runtime prints a note saying so.

Link through `weavec-cc`; a link by another driver or by `ld` directly fails with undefined `__weavec_rt_` symbols. The build compiles without guards and links without the runtime when the command line has a sanitizer that replaces the allocator (`-fsanitize=address`, `hwaddress`, `memory`, `thread`, `leak`, `kernel-address`), `-ffreestanding`, `-nostdlib`, `-nodefaultlibs` or `-nolibc`, or a target other than 64-bit Darwin or Linux. The link says so:

```text
weavec-cc: note: building without the WeaveC runtime (-fsanitize=address replaces the allocator): memory accesses are not guarded
```

A program that defines `malloc`, `calloc`, `realloc` or `free` itself keeps its allocator. The runtime is still linked and the guards stay, but the heap is untracked:

```text
weavec-cc: note: 'alloc.o' defines the allocator, so the WeaveC runtime's is not linked: the heap is untracked, guards pass on it and releases are not validated
```

An allocator that poisons its free space through ASan's interface (`ASAN_POISON_MEMORY_REGION`) gets the same protection under `weavec-cc`: an access to poisoned bytes reports `use-after-poison`.

Objects compiled by `weavec-cc` and objects from another compiler link together. The other objects' code is not guarded; their calls into guarded code, and the memory they allocate through `malloc`, are covered as usual. The [command-line reference](/reference/cli/#the-runtime) lists the environment variables, and [safety guarantees](/reference/guarantees/) says what a guard promises.

## Add checks to CI

Build and test with `weavec-cc` as the compiler:

```sh
cmake -S . -B build-weavec -G Ninja -DCMAKE_C_COMPILER=weavec-cc \
  -DCMAKE_C_FLAGS="-fweavec-ledger=$PWD/build-weavec/ledger/"
cmake --build build-weavec
ctest --test-dir build-weavec
```

Let a nonzero exit status fail the job: an error from the build, or a trap in a test. If tests capture standard error, set `WEAVEC_RT_REPORT_LOG=<path>` so the reports land in a file you can upload. Tests run slower under the guarded build than under a plain one; budget for it.
