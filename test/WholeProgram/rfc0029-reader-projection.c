// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/readers/client.c %S/../evaluation/rfc0029/readers/cursor.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/readers/short.c %S/../evaluation/rfc0029/readers/cursor.c -- 2>&1 | FileCheck %s --check-prefix=SHORT
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/readers/uninitialized.c %S/../evaluation/rfc0029/readers/cursor.c -- 2>&1 | FileCheck %s --check-prefix=TAIL
// CLEAN-NOT: error:
// SHORT: cannot establish checked safety: callee buffer allocation extent and initialized-prefix precondition must hold [weavec::checking-incomplete]
// TAIL: cannot establish checked safety: callee buffer allocation extent and initialized-prefix precondition must hold [weavec::checking-incomplete]
// RFC 0029: a changing field cursor requires the whole iteration envelope.
