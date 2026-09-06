// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(void) {
  char *p = malloc(4);
  if (!p) return;
  struct box *b = malloc(sizeof *b);
  if (!b) { free(p); return; }
  b->data = p;
  free(p);
  b->data[0] = 0; // BUG: alias
  free(b);
}
