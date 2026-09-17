// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/in-place-extension/recursive.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/in-place-extension/lost-child.c -- 2>&1 | FileCheck %s --check-prefix=LOST
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/in-place-extension/nondecreasing.c -- 2>&1 | FileCheck %s --check-prefix=PROGRESS
// CLEAN-NOT: error:
// LOST: 'c' is leaked [weavec::leak]
// PROGRESS: recursive proof cycle has no strict progress
// RFC 0029: the caller retains its head; every fresh descendant needs cleanup.
