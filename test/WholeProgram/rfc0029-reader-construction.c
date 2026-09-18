// RUN: %weavec --checked-function=build --checked-function=main %S/../evaluation/rfc0029/reader-construction/build.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=build --checked-function=main %S/../evaluation/rfc0029/mutable-reader-construction/build.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=build %S/../evaluation/rfc0029/reader-construction/cycle.c -- 2>&1 | FileCheck %s --check-prefix=PROGRESS
// RUN: not %weavec --checked-function=build %S/../evaluation/rfc0029/mutable-reader-construction/leak.c -- 2>&1 | FileCheck %s --check-prefix=CLEANUP
// CLEAN-NOT: error:
// PROGRESS: recursive construction does not establish its complete output contract [weavec::checking-incomplete]
// CLEANUP: container operation loses part of the owned allocation footprint [weavec::checking-incomplete]
// RFC 0029: record layout nominates roles; the initialized interval, progress
// and complete allocation footprint still require independent evidence.
