// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-helper-frames/good.c %S/../evaluation/rfc0029/byte-helper-frames/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-helper-frames/offset.c %S/../evaluation/rfc0029/byte-helper-frames/library.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/byte-helper-frames/local.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-helper-frames/changed.c %S/../evaluation/rfc0029/byte-helper-frames/library.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: complete helper writes to a confined automatic object preserve independently live separate byte inputs.
