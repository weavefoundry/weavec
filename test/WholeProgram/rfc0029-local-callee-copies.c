// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/local-callee-copies/numeric.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/local-callee-copies/copy.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/local-callee-copies/freed-alias.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: a confined local output keeps ownership, including cleanup duty.
