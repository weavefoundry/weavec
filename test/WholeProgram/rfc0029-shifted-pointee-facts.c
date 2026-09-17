// RUN: %weavec --checked-function=main %S/../evaluation/rfc0029/shifted-pointee-facts/same.c --
// RUN: not %weavec --checked-function=main %S/../evaluation/rfc0029/shifted-pointee-facts/shift.c -- 2>&1 | FileCheck %s
// CHECK: access interval must fit its object
// RFC 0029: an element displacement must not copy the original pointee value.
