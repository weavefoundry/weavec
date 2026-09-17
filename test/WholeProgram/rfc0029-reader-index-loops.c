// RUN: %weavec --checked-function=scan %S/../evaluation/rfc0029/reader-index-loops/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/reader-index-loops/library.c %S/../evaluation/rfc0029/reader-index-loops/good.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/reader-index-loops/body-index.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: strict unit-stride induction requires an unchanged index and reader.
