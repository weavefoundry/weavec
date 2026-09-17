// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-scans/good.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/numeric-scans/permissive.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: unsupported checked C construct or storage type [weavec::checking-incomplete]
// RFC 0029: numeric bytes exclude NaN only while actual contents remain proved.
