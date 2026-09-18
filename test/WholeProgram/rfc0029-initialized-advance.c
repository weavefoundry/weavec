// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/initialized-advance/good.c %S/../evaluation/rfc0029/initialized-advance/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/initialized-advance/over-advance.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: each initialized output byte needs proof on every return.
