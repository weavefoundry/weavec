// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/cast-reader-intervals/library.c %S/../evaluation/rfc0029/cast-reader-intervals/good.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/cast-reader-intervals/scaled-good.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/cast-reader-intervals/scaled-bad.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: runtime access interval must fit its object [weavec::checking-incomplete]
// RFCs 0004/0029: pointer casts preserve the original scaled byte offset.
