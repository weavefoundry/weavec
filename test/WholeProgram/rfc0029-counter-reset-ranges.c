// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/counter-reset-ranges-reviewed/good.c %S/../evaluation/rfc0029/counter-reset-ranges-reviewed/library.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/counter-reset-ranges-reviewed/released.c -- 2>&1 | FileCheck %s
// CHECK: after it was freed
// RFC 0029: a reset index cannot destroy the unchanged count's proved range.
