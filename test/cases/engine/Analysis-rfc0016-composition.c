// Engine pin converted from test/Analysis/rfc0016-composition.c; markers are the v0.10.0 golden diagnostics.
// RFC 0016: one source operation versus several paths, and ordered effects.
#include "Inputs/prelude.h"

static void after(char *a, char *b) {
  free(a);
  *b = 1; // BUG: use-after-free
}
static void twice(char *a, char *b) {
  free(a);
  free(b); // BUG: double-free
}
static void after_output(char **a, char **b) {
  free(*a);
  **b = 1; // BUG: use-after-free
}
static void forward(char *a, char *b) { after(a, b); }
void bad(void) {
  char *p = malloc(4);
  if (!p) return;
  forward(p, p);
  p = malloc(4);
  if (!p) return;
  twice(p, p);
  p = malloc(4);
  if (!p) return;
  after_output(&p, &p);
}
