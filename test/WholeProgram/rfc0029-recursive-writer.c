// RUN: %weavec --checked-function=emit --checked-function=main %S/../evaluation/rfc0029/recursive-writer-reviewed/prefix.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=emit --checked-function=other --checked-function=main %S/../evaluation/rfc0029/recursive-writer-reviewed/mutual.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=emit %S/../evaluation/rfc0029/recursive-writer-reviewed/false-prefix.c -- 2>&1 | FileCheck %s --check-prefix=OUTPUT
// CLEAN-NOT: error:
// OUTPUT: recursive writer does not establish its complete output contract [weavec::checking-incomplete]
// RFC 0029: a returning writer must prove its actual initialized prefix.
