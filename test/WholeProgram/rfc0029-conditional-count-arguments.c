// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/conditional-count-arguments/good.c %S/../evaluation/rfc0029/conditional-count-arguments/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/conditional-count-arguments/changed.c %S/../evaluation/rfc0029/conditional-count-arguments/library.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: each initialized output byte needs proof on every return.
