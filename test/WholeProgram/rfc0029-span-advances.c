// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/span-advances/good.c %S/../evaluation/rfc0029/span-advances/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/span-advances/over-step.c %S/../evaluation/rfc0029/span-advances/library.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: each returned step must fit the actual captured remaining span.
