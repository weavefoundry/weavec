// RFC 0005 reported `annotation-required` once per program for a callee no
// unit defines. RFC 0030 §5.1 removes the warning: every call to such a
// callee is not proven (`unresolved(unknown-callee)`), and a callee another
// unit of the program defines is known to the whole-program analysis (in a
// per-unit analysis it is unknown too).
//
// RUN: %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: %weavec %s -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ALONE %s
#include "../Inputs/prelude.h"
#include "node.h"

struct blob;
struct blob *blob_open(const char *path);
void blob_close(struct blob *b);

// CHECK-NOT: warning:
// Whole program: only the four calls to the `blob_` functions.
// CHECK: rfc0005-boundary-once.c: 9 sites: 5 proven, 4 not proven, 0 violations, 0 trusted; 0 errors, 0 warnings
// ALONE-NOT: warning:
// ALONE: rfc0005-boundary-once.c: 9 sites: 3 proven, 6 not proven, 0 violations, 0 trusted; 0 errors, 0 warnings

void first_caller(void) {
  struct blob *b = blob_open("x");
  blob_close(b);
}

void second_caller(void) {
  struct blob *b = blob_open("y");
  blob_close(b);
}

// Defined in node.c: unknown to this unit alone.
void uses_the_library(void) {
  struct node *n = node_new();
  node_free(n);
}
