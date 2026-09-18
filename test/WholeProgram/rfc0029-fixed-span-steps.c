// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/fixed-span-steps/runtime.c %S/../evaluation/rfc0029/fixed-span-steps/library.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/fixed-span-steps/over-count.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: formed pointer must remain within its object or one past it [weavec::checking-incomplete]
// RFC 0029: constant steps retain a verified span bound, never an overstated count.
