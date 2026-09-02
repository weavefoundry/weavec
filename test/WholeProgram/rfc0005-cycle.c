// RFC 0005: units that depend on each other form a cyclic component and are
// iterated to a fixpoint before anything is reported. `a_free` here calls
// `b_free` in cycle-b.c, and cycle-b.c calls `a_free`; both frees must be
// known for the double free in each unit to be found.
//
// RUN: not %weavec --whole-program %s %S/Inputs/cycle-b.c -- 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program %s %S/Inputs/cycle-b.c -- 2>&1 | FileCheck --check-prefix=B %s
#include "../Inputs/prelude.h"

void b_free(void *p);

void a_free(void *p) { b_free(p); }

int a_use_after(void) {
  char *p = malloc(1);
  a_free(p);
  // CHECK: rfc0005-cycle.c:[[@LINE+1]]:10: error: use of 'p' after it was freed [weavec::use-after-free]
  return p[0];
}

// B: cycle-b.c:[[#]]:3: error: 'p' is freed twice [weavec::double-free]
