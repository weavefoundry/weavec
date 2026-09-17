// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/container-value-guards/null-result.c --
// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/container-value-guards/replaced-head.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/container-value-guards/changed-alias.c -- 2>&1 | FileCheck %s
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/container-value-guards/replaced-live-head.c -- 2>&1 | FileCheck %s
// CHECK: error: cannot establish checked safety: access interval must fit its object [weavec::checking-incomplete]
// RFC 0029: a current head predicate supplies a value, and mutation retires it.
