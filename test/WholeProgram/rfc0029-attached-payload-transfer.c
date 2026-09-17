// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/attached-payload-transfer/good.c %S/../evaluation/rfc0029/attached-payload-transfer/library.c --
// RUN: not %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/attached-payload-transfer/leak.c %S/../evaluation/rfc0029/attached-payload-transfer/library.c -- 2>&1 | FileCheck %s
// CHECK: cannot establish checked safety: container operation loses part of the owned allocation footprint
// RFC 0029: a successful attach transfers both owned inputs plus its own
// fresh payload; the failure path keeps them separate, so losing the item
// on that path remains a rejection.
