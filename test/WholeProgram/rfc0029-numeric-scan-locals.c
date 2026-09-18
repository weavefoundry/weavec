// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-scan-locals/assignment.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-scan-locals/source-write.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: unsupported checked C construct or storage type [weavec::checking-incomplete]
// RFC 0029: private scalar writes preserve byte facts; source writes retire them.
