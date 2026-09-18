// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-early-exits/client.c %S/../evaluation/rfc0029/payload-early-exits/goto.c %S/../evaluation/rfc0029/payload-early-exits/drop.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-early-exits/client.c %S/../evaluation/rfc0029/payload-early-exits/early.c %S/../evaluation/rfc0029/payload-early-exits/drop.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-early-exits/client.c %S/../evaluation/rfc0029/payload-early-exits/released-head.c %S/../evaluation/rfc0029/payload-early-exits/drop.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
