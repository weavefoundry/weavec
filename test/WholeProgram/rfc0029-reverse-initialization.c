// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/reverse-initialization/good.c %S/../evaluation/rfc0029/reverse-initialization/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/reverse-initialization/skipped.c  -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: each initialized output byte needs proof on every return.
