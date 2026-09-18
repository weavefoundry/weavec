// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-short-circuit/left.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-short-circuit/bypass.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: unsupported checked C construct or storage type [weavec::checking-incomplete]
// RFC 0029: short-circuit tests retain only the already-visited numeric prefix.
