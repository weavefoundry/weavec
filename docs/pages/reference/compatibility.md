---
title: Compatibility
description: Supported toolchains, source portability, external code, object sidecars, and current distribution boundaries.
---

## C and Clang

WeaveC uses Clang for C parsing, semantic analysis, target layout, optimization, and code generation. Pass the language standard and target flags used by your project. Clang accepting a construct does not mean WeaveC can establish every safety obligation for it.

Use [checker coverage](/reference/checker-coverage/) and [checked limits](/reference/checked-limits/) to distinguish language support from analysis coverage.

## Annotations remain portable

Annotations live in `weavec.h`. Under Clang they use the `annotate` attribute; under compilers without that attribute they expand away. Check [annotation placement](/reference/annotation-placement/) for syntax and [compatibility with other annotation schemes](/reference/annotation-compatibility/) for current integration boundaries.

`WEAVEC_ASSUME` has special evaluation behavior. Read its reference entry before placing an expression in it; assumptions must be side-effect free and are trusted.

## LLVM versions and platforms

The repository supports LLVM/Clang development installations from version 20, with version 23 recommended. The release CI exercises Linux and macOS. A C++20 compiler is required to build WeaveC itself.

Current releases are source archives. Portable binary packages and package-manager distribution are future work. A built compiler records paths to its LLVM installation, which must remain available at runtime.

## External code and object files

The shipped libc/POSIX table models many common allocation, release, string, and runtime operations. Unmodeled dependencies remain boundaries. General C++ analysis and automatic proof of arbitrary external library implementations are outside this C-focused workflow.

Keep `.o.weavec` files beside the corresponding objects. Rebuild after relevant source, header, command, or metadata changes. Static archives do not yet automatically carry their members' sidecars.

## Version changes

WeaveC is in early 0.x development. APIs and serialized summary, sidecar, and checkpoint formats can change between minor versions. Check the [release notes](/project/releases/) before upgrading and rebuild affected objects and caches.
