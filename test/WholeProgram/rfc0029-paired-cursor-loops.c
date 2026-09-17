// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/paired-cursor-loops/good.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/paired-cursor-loops/over-step.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: an earlier scan bounds each unit step of the following cursor.
