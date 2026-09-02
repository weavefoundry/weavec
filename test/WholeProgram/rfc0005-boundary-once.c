// RFC 0005: `annotation-required` fires only for callees no unit of the
// program defines, and once per callee for the whole program rather than
// once per unit that calls it.
//
// RUN: %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ONCE %s
// RUN: %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | grep "call to 'blob_open' is not checked" | count 1
// RUN: %weavec --whole-program -Wno-weavec-annotation-required %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | count 0
#include "../Inputs/prelude.h"
#include "node.h"

struct blob;
struct blob *blob_open(const char *path);
void blob_close(struct blob *b);

// ONCE: warning: call to 'blob_open' is not checked
// ONCE: warning: call to 'blob_close' is not checked
// ONCE-NOT: warning:

void first_caller(void) {
  // CHECK: rfc0005-boundary-once.c:[[@LINE+1]]:20: warning: call to 'blob_open' is not checked: it has no definition or ownership annotations here [weavec::annotation-required]
  struct blob *b = blob_open("x");
  // CHECK: note: annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it in this program
  // CHECK: rfc0005-boundary-once.c:[[@LINE+1]]:3: warning: call to 'blob_close' is not checked
  blob_close(b);
}

void second_caller(void) {
  struct blob *b = blob_open("y");
  blob_close(b);
}

// Defined in node.c: not a boundary, whatever this unit knows.
// CHECK-NOT: call to 'node_new'
void uses_the_library(void) {
  struct node *n = node_new();
  node_free(n);
}
