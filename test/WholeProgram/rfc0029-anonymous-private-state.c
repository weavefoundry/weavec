// RUN: %weavec --whole-program --checked-function=main %S/../evaluation/rfc0029/anonymous-private-state/state.c %S/../evaluation/rfc0029/anonymous-private-state/client.c -- 2>&1 | FileCheck %s --allow-empty
// CHECK-NOT: error:
// RFC 0029: anonymous typedefs retain private numeric outputs across units.
