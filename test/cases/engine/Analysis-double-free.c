// Engine pin converted from test/Analysis/double-free.c; markers are the v0.10.0 golden diagnostics.
#include "Inputs/prelude.h"

void double_free(int *p) {
  free(p);
  free(p); // BUG: double-free definite
}
