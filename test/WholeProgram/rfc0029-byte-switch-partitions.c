// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-switch-partitions-reviewed/direct.c %S/../evaluation/rfc0029/byte-switch-partitions-reviewed/scanner.c %S/../evaluation/rfc0029/byte-switch-partitions-reviewed/wrapper.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-switch-partitions-reviewed/beyond-bound.c %S/../evaluation/rfc0029/byte-switch-partitions-reviewed/scanner.c %S/../evaluation/rfc0029/byte-switch-partitions-reviewed/wrapper.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
