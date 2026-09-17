// RUN: %weavec --checked-function=probe %S/../evaluation/rfc0029/span-count-joins/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/span-count-joins/library.c %S/../evaluation/rfc0029/span-count-joins/good.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/span-count-joins/changed-count.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: assignment/guard order cannot replace all-path count evidence.
