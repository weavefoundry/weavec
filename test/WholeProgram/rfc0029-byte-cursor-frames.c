// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-frames/good.c %S/../evaluation/rfc0029/byte-cursor-frames/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-frames/offset.c %S/../evaluation/rfc0029/byte-cursor-frames/library.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/byte-cursor-frames/local.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-frames/changed.c %S/../evaluation/rfc0029/byte-cursor-frames/library.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: writes to a separate allocation and an exact local pointer cell preserve the input byte evidence; actual input mutation retires it.
