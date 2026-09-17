// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/paired-reader-counters/library.c %S/../evaluation/rfc0029/paired-reader-counters/good.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/paired-reader-counters/too-many.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: count equality needs actual equal initial values and preserved steps.
