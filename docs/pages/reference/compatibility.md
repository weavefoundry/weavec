---
title: Compatibility
description: Supported toolchains, source portability, what runtime checks and zero-initialisation change, external code, object records, and distribution boundaries.
---

## C and Clang

WeaveC uses Clang for C parsing, semantic analysis, target layout, optimization and code generation. Pass the language standard and target flags used by your project. Clang accepting a construct does not mean WeaveC can prove every safety obligation for it; what it cannot prove or check is listed in the ledger with a reason. C++ and Objective-C sources pass through to Clang unchanged.

Use [checker coverage](/reference/checker-coverage/) to see what the analysis understands.

## Annotations remain portable

Annotations live in `weavec.h`. Under Clang they use the `annotate` attribute; under compilers without that attribute they expand away. Check [annotation placement](/reference/annotation-placement/) for syntax and [compatibility with other annotation schemes](/reference/annotation-compatibility/) for current integration boundaries. WeaveC also reads Clang's own `counted_by`, `sized_by`, `alloc_size`, `nonnull` and ownership attributes.

`WEAVEC_ASSUME` has special evaluation behavior. Read its reference entry before placing an expression in it: the expression must be side-effect free, and `weavec-cc` may turn it into a runtime assertion.

## What changes in compiled code

With the default `-fweavec-checks=trap`, `weavec-cc` inserts plain C checks before code generation. There is no pointer ABI change and no runtime library in this mode (precompiled-header and module builds link a small library of out-of-line check helpers); objects link with objects from any compiler. `-fweavec-checks=none` produces the object Clang would produce.

In the checking modes, locals and the standard allocation calls are zero-initialised. This changes behavior only for programs that read indeterminate values, which C leaves undefined. Comparing a function pointer with `malloc` sees WeaveC's zero-initialising wrapper and is false; a unit that defines its own `malloc`, `calloc`, `realloc` or `free` is not rewritten. `-fno-weavec-zero-init` turns zero-initialisation off.

A correct program that relies on undefined behavior that happens to work, such as reading one element past an array, can trap. Keep such code in a narrow `WEAVEC_UNSAFE` region.

## LLVM versions and platforms

The repository supports LLVM/Clang development installations from version 20, with version 23 recommended. The release CI exercises Linux and macOS. A C++20 compiler is required to build WeaveC itself.

Current releases are source archives. Portable binary packages and package-manager distribution are future work. A built compiler records paths to its LLVM installation, which must remain available at runtime.

## External code and object files

The library table (`lib/Core/LibrarySpec.txt`) describes the C library, POSIX and platform functions WeaveC models: allocation, release, string, stream and runtime operations. Other functions declared in platform headers are assumed to borrow their arguments and are trusted. Functions WeaveC cannot see at all are treated conservatively as possibly freeing or keeping their pointer arguments, and are listed in the ledger.

Keep `.o.weavec` records beside the corresponding objects and rebuild after source, header or command changes. Link inputs without a valid record, including static archives and shared libraries, are named in one `unanalyzed-input` warning, and calls into them are trusted.

## Version changes

WeaveC is in early 0.x development. Flags, diagnostics, the ledger schema and the object record format can change between minor versions; a record from an incompatible version is treated as missing. Check the [release notes](/project/releases/) before upgrading and rebuild affected objects.
