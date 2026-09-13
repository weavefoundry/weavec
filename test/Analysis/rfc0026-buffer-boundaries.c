// RUN: %weavec --checked-function=main %S/../evaluation/rfc0026/runtime-append.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0026/capacity-lie-bad.c -- 2>&1 | FileCheck %s --check-prefix=CAPACITY
// RUN: not %weavec --checked-function=main %s -- 2>&1 | FileCheck %s --check-prefix=BACKING
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0026/owned-vector-skipped-bad.c -- 2>&1 | FileCheck %s --check-prefix=ELEMENTS
// RFC 0026: the frozen clients pin the distinction between advertised counts,
// allocation extent, initialized contents and allocation release permission.
// CLEAN-NOT: error:
// CAPACITY: error: cannot establish checked safety: callee buffer allocation extent and initialized-prefix precondition must hold [weavec::checking-incomplete]
// ELEMENTS: error: cannot establish checked safety: owned buffer elements require complete release or transfer before container mutation [weavec::checking-incomplete]
// BACKING: error: cannot establish checked safety: callee buffer backing storage requires allocation release permission [weavec::checking-incomplete]

#include "../evaluation/rfc0026/buffer.h"
int main(void) {
  unsigned char local[16] = {0};
  struct buffer b = {local, 1, 16};
  return reserve(&b, 32);
}
