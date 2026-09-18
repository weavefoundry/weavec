// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/bounded-string-cursor/finish.c %S/../evaluation/rfc0029/bounded-string-cursor/bounded.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/bounded-string-cursor/finish.c %S/../evaluation/rfc0029/bounded-string-cursor/earlier-zero.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/bounded-string-cursor/finish.c %S/../evaluation/rfc0029/bounded-string-cursor/uninitialized.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: callee terminated-within safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: strlen finds the first zero in an initialized bounded prefix.
// Neither a capacity field nor an isolated later zero initializes that prefix.
