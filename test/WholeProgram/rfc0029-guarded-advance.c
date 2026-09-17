// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/guarded-advance/good.c %S/../evaluation/rfc0029/guarded-advance/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/guarded-advance/changed.c %S/../evaluation/rfc0029/guarded-advance/library.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: the actual unmodified result must establish the cursor guarantee.
