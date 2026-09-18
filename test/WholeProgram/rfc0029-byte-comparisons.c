// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-comparisons/bom-absent.c %S/../evaluation/rfc0029/byte-comparisons/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-comparisons/embedded-zero-memory.c %S/../evaluation/rfc0029/byte-comparisons/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-comparisons/unsigned-order.c %S/../evaluation/rfc0029/byte-comparisons/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-comparisons/bom-present.c %S/../evaluation/rfc0029/byte-comparisons/library.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: actual initialized bytes establish only the C comparison result's sign.
