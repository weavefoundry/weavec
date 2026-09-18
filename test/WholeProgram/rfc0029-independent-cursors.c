// RUN: %weavec --checked-function=copy_bytes %S/../evaluation/rfc0029/independent-cursors/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/independent-cursors/good.c %S/../evaluation/rfc0029/independent-cursors/library.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/independent-cursors/extra-step.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: access interval must fit its object [weavec::checking-incomplete]
// RFC 0029: independently proved numeric cursor relations grant no pointer identity.
