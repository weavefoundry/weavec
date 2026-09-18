// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-static/good.c %S/../evaluation/rfc0029/byte-cursor-static/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-static/offset.c %S/../evaluation/rfc0029/byte-cursor-static/library.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/byte-cursor-static/local.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/byte-cursor-static/changed.c %S/../evaluation/rfc0029/byte-cursor-static/library.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: callee extent safety precondition must hold [weavec::checking-incomplete]
// RFC 0029: immutable static local initializers establish contents; mutable static storage does not regain stale contents.
