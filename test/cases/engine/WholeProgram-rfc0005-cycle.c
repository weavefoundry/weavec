// Engine pin converted from test/WholeProgram/rfc0005-cycle.c; markers are the v0.10.0 golden diagnostics.
// UNITS: Inputs/cycle-b.c
// RFC 0005: units that depend on each other form a cyclic component and are
// iterated to a fixpoint before anything is reported. `a_free` here calls
// `b_free` in cycle-b.c, and cycle-b.c calls `a_free`; both frees must be
// known for the double free in each unit to be found.
//
#include "Inputs/prelude.h"

void b_free(void *p);

void a_free(void *p) { b_free(p); }

int a_use_after(void) {
  char *p = malloc(1);
  a_free(p);
  return p[0]; // BUG: use-after-free
}
