// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-relocation/helper.c %S/../evaluation/rfc0029/payload-relocation/fill.c %S/../evaluation/rfc0029/payload-relocation/drop.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/payload-relocation/duplicate.c %S/../evaluation/rfc0029/payload-relocation/fill.c %S/../evaluation/rfc0029/payload-relocation/drop.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
