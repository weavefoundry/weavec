// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/pointer-difference-sizes/good.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/pointer-difference-sizes/offset.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/pointer-difference-sizes/undersized.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: access interval must fit its object [weavec::checking-incomplete]
// RFC 0029: pointer differences retain their validated coordinate and allocation-time identity.
