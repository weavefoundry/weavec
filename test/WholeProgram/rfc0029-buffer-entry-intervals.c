// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/buffer-entry-intervals-reviewed/spare-physical.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/buffer-entry-intervals-reviewed/short-physical.c -- 2>&1 | FileCheck %s --check-prefix=SHORT
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/buffer-entry-intervals-reviewed/uninitialized-tail.c -- 2>&1 | FileCheck %s --check-prefix=UNINITIALIZED
// CLEAN-NOT: error:
// SHORT: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// UNINITIALIZED: cannot establish checked safety: callee initialized safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: capacity is a lower bound. Additional entry bytes must be proved.
