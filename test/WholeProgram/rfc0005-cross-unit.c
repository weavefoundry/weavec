// RFC 0005: with --whole-program, a call to a function defined in another
// unit is checked against that definition's summary instead of being a
// boundary. Bugs that need two files are found, and no `annotation-required`
// is reported for callees the program defines.
//
// RUN: not %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=NONE %s
//
// Order of the sources on the command line does not matter: node.c is a
// dependency and is analysed first either way.
// RUN: not %weavec --whole-program %S/Inputs/node.c %s -- -I%S/Inputs 2>&1 | FileCheck %s
//
// Without --whole-program the same file is its own program and the calls
// are boundaries (RFC 0003), so nothing is an error.
// RUN: %weavec %s -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ALONE %s
#include "../Inputs/prelude.h"
#include "node.h"

// NONE-NOT: annotation-required
// ALONE: warning: call to 'node_new' is not checked
// ALONE-NOT: error:

int double_release(void) {
  struct node *n = node_new();
  node_free(n);
  // CHECK: rfc0005-cross-unit.c:[[@LINE+1]]:3: error: 'n' is freed twice [weavec::double-free]
  node_free(n);
  return 0;
}

int dangling_field_pointer(void) {
  struct node *n = node_new();
  int *p = node_vp(n);
  // CHECK: rfc0005-cross-unit.c:[[@LINE+1]]:3: error: cannot free 'n' while it is borrowed [weavec::conflicting-borrow]
  node_free(n);
  return *p;
}

int fine(void) {
  struct node *n = node_new();
  if (!n)
    return 1;
  *node_vp(n) = 3;
  node_set_name(n, malloc(4));
  node_free(n);
  return 0;
}
