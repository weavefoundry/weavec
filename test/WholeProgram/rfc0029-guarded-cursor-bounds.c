// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/guarded-cursor-bounds/good.c %S/../evaluation/rfc0029/guarded-cursor-bounds/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/guarded-cursor-bounds/changed.c %S/../evaluation/rfc0029/guarded-cursor-bounds/library.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: the actual unmodified result must establish the cursor guarantee.
