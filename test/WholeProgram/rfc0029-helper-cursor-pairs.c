// RUN: %weavec --checked-function=decode %S/../evaluation/rfc0029/helper-cursor-pairs/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/helper-cursor-pairs/good.c %S/../evaluation/rfc0029/helper-cursor-pairs/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/helper-cursor-pairs/mixed.c %S/../evaluation/rfc0029/helper-cursor-pairs/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/helper-cursor-pairs/short.c %S/../evaluation/rfc0029/helper-cursor-pairs/library.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: helper cursor offsets must survive every loop path; zero bytes refute only actual nonzero reads.
