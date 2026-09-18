// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-global-frames/good.c %S/../evaluation/rfc0029/byte-global-frames/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-global-frames/offset.c %S/../evaluation/rfc0029/byte-global-frames/library.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/byte-global-frames/local.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-global-frames/changed.c %S/../evaluation/rfc0029/byte-global-frames/library.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: actual constant array bytes survive separate global writes; const pointer spelling supplies no immutable object premise.
