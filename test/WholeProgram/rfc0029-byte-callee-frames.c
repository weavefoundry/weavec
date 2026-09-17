// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-callee-frames/static.c %S/../evaluation/rfc0029/byte-callee-frames/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-callee-frames/wrong.c %S/../evaluation/rfc0029/byte-callee-frames/library.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
