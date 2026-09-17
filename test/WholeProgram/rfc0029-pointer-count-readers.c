// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/pointer-count-readers/good.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/pointer-count-readers/short.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: inferred pointer/count input intervals remain caller obligations.
