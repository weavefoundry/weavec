// RUN: %weavec --checked-function=probe %S/../evaluation/rfc0029/span-counts/library.c --
// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/span-counts/library.c %S/../evaluation/rfc0029/span-counts/good.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/span-counts/false-count.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety:
// RFC 0029: count bounds are proved at every return against entry endpoints.
