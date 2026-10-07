// RFC 0005: with --whole-program, a call to a function defined in another
// unit is checked against that definition's summary instead of being a
// boundary. Bugs that need two files are found.
//
// RUN: not %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=NONE %s
//
// Order of the sources on the command line does not matter: node.c is a
// dependency and is analysed first either way.
// RUN: not %weavec --whole-program %S/Inputs/node.c %s -- -I%S/Inputs 2>&1 | FileCheck %s
//
// Without --whole-program the same file is its own program and the calls
// are into unknown code (RFC 0030 §5.1): not proven, and nothing is
// reported.
// RUN: %weavec %s -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ALONE %s
#include "../Inputs/prelude.h"
#include "node.h"

// NONE-NOT: warning: call
// ALONE-NOT: {{warning|error}}:
// ALONE: 0 errors, 0 warnings

// `node_new` may return null and `node_free` releases only a non-null
// argument, so the bug is definite once `n` is tested (RFC 0031 §6.2:
// `release *param0 free when param 0 !=0`); untested, it is a warning. The
// callee reads `n->name` before it frees `n`, so the first invalid
// operation of the second call is that read: a use after free of the
// argument, reported at it.
int double_release(void) {
  struct node *n = node_new();
  if (!n)
    return 1;
  node_free(n);
  // CHECK: rfc0005-cross-unit.c:[[@LINE+1]]:13: error: use of 'n' after it was freed [weavec::use-after-free]
  node_free(n);
  return 0;
}

int dangling_field_pointer(void) {
  struct node *n = node_new();
  if (!n)
    return 1;
  int *p = node_vp(n);
  node_free(n);
  // `node_vp` returns a copy of `n` at the field `v` (RFC 0011): the
  // dangling read is the report.
  // CHECK: rfc0005-cross-unit.c:[[@LINE+1]]:11: error: use of 'p' after it was freed [weavec::use-after-free]
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
