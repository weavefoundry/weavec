// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/advance-outcomes/good.c %S/../evaluation/rfc0029/advance-outcomes/library.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/advance-outcomes/bad-failure.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: every return outcome must justify its advanced bytes.
