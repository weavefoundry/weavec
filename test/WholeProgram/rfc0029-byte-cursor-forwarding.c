// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-forwarding/good.c %S/../evaluation/rfc0029/byte-cursor-forwarding/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-forwarding/offset.c %S/../evaluation/rfc0029/byte-cursor-forwarding/library.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/byte-cursor-forwarding/local.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-forwarding/changed.c %S/../evaluation/rfc0029/byte-cursor-forwarding/library.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: a read-only wrapper transports actual initialized byte contents and their proved readable interval to the checked scan.
