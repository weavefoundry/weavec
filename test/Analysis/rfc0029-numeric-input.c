// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-input/end.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-input/null-end.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-input/readonly-slot.c -- 2>&1 | FileCheck %s --check-prefix=READONLY
// CLEAN-NOT: error:
// READONLY: checked safety failed: cannot write to read-only storage [weavec::checking-failed]
// RFC 0029: an end pointer keeps the input object's bounds and lifetime;
// no finite floating-point result is inferred from the library call.
