// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-content/good.c %S/../evaluation/rfc0029/byte-cursor-content/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-content/offset.c %S/../evaluation/rfc0029/byte-cursor-content/library.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/byte-cursor-content/local.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-content/changed.c %S/../evaluation/rfc0029/byte-cursor-content/library.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: actual byte contents refine scans only while their initialized storage and contents remain current.
