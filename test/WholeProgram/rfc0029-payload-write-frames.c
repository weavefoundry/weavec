// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-write-frames/client.c %S/../evaluation/rfc0029/payload-write-frames/good.c %S/../evaluation/rfc0029/payload-write-frames/drop.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-write-frames/client.c %S/../evaluation/rfc0029/payload-write-frames/helper.c %S/../evaluation/rfc0029/payload-write-frames/drop.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-write-frames/aliased-client.c %S/../evaluation/rfc0029/payload-write-frames/good.c %S/../evaluation/rfc0029/payload-write-frames/drop.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee requires separated input objects [weavec::checking-incomplete]
// RFC 0029: a represented cursor store must be separate from the incoming head.
