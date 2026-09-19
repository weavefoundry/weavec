// Engine pin converted from test/Analysis/use-after-free.c; markers are the v0.10.0 golden diagnostics.
#include "Inputs/prelude.h"

void use_after_free(void) {
  int *p = malloc(sizeof(int));
  free(p);
  *p = 1; // BUG: use-after-free
}

void use_via_call(void) {
  int *p = malloc(sizeof(int));
  free(p);
  use(p); // BUG: use-after-free
}
