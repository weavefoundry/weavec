// RUN: %weavec --checked-function=visit_even --checked-function=visit_odd --checked-function=main %S/../evaluation/rfc0029/traversal/mutual.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=visit_even --checked-function=visit_odd --checked-function=main %S/../evaluation/rfc0029/traversal/nondecreasing.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=visit_even %S/../evaluation/rfc0029/traversal/cycle.c -- 2>&1 | FileCheck %s --check-prefix=CYCLE
// CLEAN-NOT: error:
// CYCLE: cannot establish checked safety: recursive proof cycle has no strict progress [weavec::checking-incomplete]
// RFC 0029: every cycle needs a strict edge; a forwarding member is allowed.
