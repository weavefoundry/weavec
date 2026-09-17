// RUN: %weavec --checked-function=scan %S/../evaluation/rfc0029/pointer-reader-offsets/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/pointer-reader-offsets/library.c %S/../evaluation/rfc0029/pointer-reader-offsets/good.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/pointer-reader-offsets/unrelated.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: pointer difference or ordering needs shared-object evidence [weavec::checking-incomplete]
// RFC 0029: pointer-difference guard projection requires established operands.
