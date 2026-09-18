// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-loop-partitions/good.c %S/../evaluation/rfc0029/byte-loop-partitions/while.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-loop-partitions/good.c %S/../evaluation/rfc0029/byte-loop-partitions/do.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-loop-partitions/good.c %S/../evaluation/rfc0029/byte-loop-partitions/skip.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: access interval must fit its object
