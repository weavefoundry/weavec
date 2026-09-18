// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/record-arrays/spellings.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/record-arrays/forward.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=array %S/../evaluation/rfc0029/zero-counters/array.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/record-arrays/alias-bad.c -- 2>&1 | FileCheck %s --check-prefix=ALIAS
// RUN: not %weavec --checked-function=partial %S/../evaluation/rfc0029/zero-counters/partial.c -- 2>&1 | FileCheck %s --check-prefix=PARTIAL
// CLEAN-NOT: error:
// ALIAS: [weavec::double-free]
// PARTIAL: cannot establish checked safety: access interval must fit its object [weavec::checking-incomplete]
// RFCs 0015/0029: exact record-array spellings agree. Current complete zero
// representations can establish counters; partial bytes and aliased releases
// retain their ordinary proof obligations.
